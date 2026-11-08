#include "config-appearance.h"

#include <glib/gstdio.h>
#include <string.h>

typedef struct {
    char *dir;
    char *config;
} Fixture;

static void
fixture_setup (Fixture *f, gconstpointer data)
{
    (void) data;
    f->dir = g_dir_make_tmp ("corral-appearance-XXXXXX", NULL);
    g_assert_nonnull (f->dir);
    f->config = g_build_filename (f->dir, "config", NULL);
}

static void
remove_tree (const char *path)
{
    GDir *dir = g_dir_open (path, 0, NULL);
    if (dir != NULL) {
        const char *name;
        while ((name = g_dir_read_name (dir)) != NULL) {
            char *child = g_build_filename (path, name, NULL);
            remove_tree (child);
            g_free (child);
        }
        g_dir_close (dir);
    }
    g_remove (path);
}

static void
fixture_teardown (Fixture *f, gconstpointer data)
{
    (void) data;
    remove_tree (f->dir);
    g_free (f->dir);
    g_free (f->config);
}

static void
write_file (const char *path, const char *body)
{
    g_assert_true (g_file_set_contents (path, body, -1, NULL));
}

static void
load (Fixture *f, CorralAppearance *a, const char *body, gboolean dark)
{
    write_file (f->config, body);
    const char *files[] = { f->config, NULL };
    const char *dirs[] = { BUNDLED_THEMES_DIR, NULL };
    corral_appearance_init (a);
    corral_appearance_load (a, files, dirs, dark);
}

static void
assert_rgb (GhosttyColorRgb got, guint8 r, guint8 g, guint8 b)
{
    g_assert_cmpuint (got.r, ==, r);
    g_assert_cmpuint (got.g, ==, g);
    g_assert_cmpuint (got.b, ==, b);
}

static void
test_bundled_theme (Fixture *f, gconstpointer data)
{
    (void) data;
    CorralAppearance a;
    load (f, &a, "theme = Dracula\n", TRUE);
    assert_rgb (a.background, 0x28, 0x2a, 0x36);
    assert_rgb (a.foreground, 0xf8, 0xf8, 0xf2);
    assert_rgb (a.palette[1], 0xff, 0x55, 0x55);
    g_assert_true (GHOSTTY_COLOR_PALETTE_MASK_IS_SET (&a.palette_set, 1));
    g_assert_cmpint (a.selection_background.kind, ==, CORRAL_TERM_COLOR_RGB);
    assert_rgb (a.selection_background.rgb, 0x44, 0x47, 0x5a);
    corral_appearance_clear (&a);
}

static void
test_user_overrides_theme (Fixture *f, gconstpointer data)
{
    (void) data;
    CorralAppearance a;
    /* Order does not matter: the theme sits under the whole file. */
    load (f, &a, "background = #010203\ntheme = Dracula\n", TRUE);
    assert_rgb (a.background, 0x01, 0x02, 0x03);
    assert_rgb (a.foreground, 0xf8, 0xf8, 0xf2);
    corral_appearance_clear (&a);
}

static void
test_theme_beside_config (Fixture *f, gconstpointer data)
{
    (void) data;
    char *themes = g_build_filename (f->dir, "themes", NULL);
    g_mkdir (themes, 0700);
    char *mine = g_build_filename (themes, "Dracula", NULL);
    write_file (mine, "background = 102030\n");
    write_file (f->config, "theme = Dracula\n");
    const char *files[] = { f->config, NULL };
    const char *dirs[] = { themes, BUNDLED_THEMES_DIR, NULL };
    CorralAppearance a;
    corral_appearance_init (&a);
    corral_appearance_load (&a, files, dirs, TRUE);
    assert_rgb (a.background, 0x10, 0x20, 0x30);
    assert_rgb (a.foreground, 0xff, 0xff, 0xff);
    corral_appearance_clear (&a);
    g_free (mine);
    g_free (themes);
}

static void
test_light_dark_theme (Fixture *f, gconstpointer data)
{
    (void) data;
    CorralAppearance a;
    const char *body = "theme = light:3024 Day,dark:Dracula\n";
    load (f, &a, body, TRUE);
    assert_rgb (a.background, 0x28, 0x2a, 0x36);
    corral_appearance_clear (&a);
    load (f, &a, body, FALSE);
    g_assert_false (a.background.r == 0x28 && a.background.g == 0x2a);
    corral_appearance_clear (&a);
}

static void
test_scrollback (Fixture *f, gconstpointer data)
{
    (void) data;
    CorralAppearance a;
    load (f, &a, "", FALSE);
    g_assert_cmpuint (a.scrollback_limit_bytes, ==, 50000000);
    g_assert_cmpuint (a.scrollback_limit_lines, ==, G_MAXSIZE);
    corral_appearance_clear (&a);

    load (f, &a, "scrollback-limit = 1234\n", FALSE);
    g_assert_cmpuint (a.scrollback_limit_bytes, ==, 1234);
    corral_appearance_clear (&a);

    load (f, &a, "scrollback-limit-bytes = unlimited\nscrollback-limit-lines = 500\n", FALSE);
    g_assert_cmpuint (a.scrollback_limit_bytes, ==, G_MAXSIZE);
    g_assert_cmpuint (a.scrollback_limit_lines, ==, 500);
    corral_appearance_clear (&a);
}

static void
assert_defaults (const CorralAppearance *a)
{
    CorralAppearance d;
    corral_appearance_init (&d);
    assert_rgb (a->background, 0x28, 0x2c, 0x34);
    assert_rgb (a->foreground, 0xff, 0xff, 0xff);
    g_assert_cmpmem (a->palette, sizeof a->palette, d.palette, sizeof d.palette);
    g_assert_cmpuint (a->palette_set.bits[0] | a->palette_set.bits[1] |
                      a->palette_set.bits[2] | a->palette_set.bits[3], ==, 0);
    g_assert_cmpint (a->cursor_style, ==, d.cursor_style);
    g_assert_cmpuint (a->padding_left, ==, d.padding_left);
    g_assert_cmpstr (a->term, ==, d.term);
    g_assert_cmpuint (a->env->len, ==, 0);
    corral_appearance_clear (&d);
}

static void
test_font_and_unknown_ignored (Fixture *f, gconstpointer data)
{
    (void) data;
    CorralAppearance a;
    load (f, &a,
          "font-family = Comic Sans\n"
          "font-size = 40\n"
          "keybind = ctrl+q=quit\n"
          "window-decoration = none\n"
          "not-a-ghostty-key = 1\n",
          FALSE);
    assert_defaults (&a);
    corral_appearance_clear (&a);
}

static void
test_missing_theme (Fixture *f, gconstpointer data)
{
    (void) data;
    CorralAppearance a;
    g_test_expect_message (G_LOG_DOMAIN, G_LOG_LEVEL_MESSAGE,
                           "*theme \"No Such Theme\" not found*");
    load (f, &a, "theme = No Such Theme\n", FALSE);
    g_test_assert_expected_messages ();
    assert_defaults (&a);
    corral_appearance_clear (&a);
}

static void
test_options (Fixture *f, gconstpointer data)
{
    (void) data;
    CorralAppearance a;
    load (f, &a,
          "# comment\n"
          "palette = 4 = #0000ff\n"
          "cursor-color = cell-foreground\n"
          "cursor-style = bar\n"
          "cursor-style-blink = false\n"
          "window-padding-x = 4,6\n"
          "window-padding-y = 8\n"
          "window-padding-balance = equal\n"
          "adjust-cell-height = 20%\n"
          "copy-on-select = clipboard\n"
          "mouse-scroll-multiplier = precision:0.5,discrete:2\n"
          "clipboard-read = deny\n"
          "term = \"xterm-256color\"\n"
          "shell-integration-features = sudo,no-title\n"
          "env = FOO=bar\n"
          "env = BAZ=1\n",
          FALSE);
    assert_rgb (a.palette[4], 0, 0, 0xff);
    g_assert_cmpint (a.cursor_color.kind, ==, CORRAL_TERM_COLOR_CELL_FOREGROUND);
    g_assert_cmpint (a.cursor_style, ==, CORRAL_CURSOR_BAR);
    g_assert_cmpint (a.cursor_style_blink, ==, 0);
    g_assert_cmpuint (a.padding_left, ==, 4);
    g_assert_cmpuint (a.padding_right, ==, 6);
    g_assert_cmpuint (a.padding_top, ==, 8);
    g_assert_cmpuint (a.padding_bottom, ==, 8);
    g_assert_cmpint (a.padding_balance, ==, CORRAL_PADDING_BALANCE_EQUAL);
    g_assert_cmpfloat (a.adjust_cell_height, ==, 20);
    g_assert_true (a.adjust_cell_height_percent);
    g_assert_cmpint (a.copy_on_select, ==, CORRAL_COPY_ON_SELECT_CLIPBOARD);
    g_assert_cmpfloat (a.mouse_scroll_precision, ==, 0.5);
    g_assert_cmpfloat (a.mouse_scroll_discrete, ==, 2);
    g_assert_cmpint (a.clipboard_read, ==, CORRAL_CLIPBOARD_DENY);
    g_assert_cmpstr (a.term, ==, "xterm-256color");
    g_assert_cmpuint (a.env->len, ==, 2);
    g_assert_cmpstr (g_ptr_array_index (a.env, 1), ==, "BAZ=1");
    char *features = corral_appearance_shell_features (&a);
    g_assert_cmpstr (features, ==, "cursor:steady,path,sudo");
    g_free (features);
    corral_appearance_clear (&a);
}

static void
test_default_shell_features (Fixture *f, gconstpointer data)
{
    (void) data;
    CorralAppearance a;
    load (f, &a, "", FALSE);
    char *features = corral_appearance_shell_features (&a);
    g_assert_cmpstr (features, ==, "cursor:blink,path,title");
    g_free (features);
    corral_appearance_clear (&a);
}

static void
test_config_file_include (Fixture *f, gconstpointer data)
{
    (void) data;
    char *extra = g_build_filename (f->dir, "extra", NULL);
    write_file (extra, "foreground = #abcdef\n");
    CorralAppearance a;
    load (f, &a, "config-file = extra\nconfig-file = ?missing\nforeground = #000000\n", FALSE);
    /* Includes load after the file that names them. */
    assert_rgb (a.foreground, 0xab, 0xcd, 0xef);
    corral_appearance_clear (&a);
    g_free (extra);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
#define ADD(path, fn) \
    g_test_add (path, Fixture, NULL, fixture_setup, fn, fixture_teardown)
    ADD ("/appearance/bundled-theme", test_bundled_theme);
    ADD ("/appearance/user-overrides-theme", test_user_overrides_theme);
    ADD ("/appearance/theme-beside-config", test_theme_beside_config);
    ADD ("/appearance/light-dark-theme", test_light_dark_theme);
    ADD ("/appearance/scrollback", test_scrollback);
    ADD ("/appearance/font-and-unknown-ignored", test_font_and_unknown_ignored);
    ADD ("/appearance/missing-theme", test_missing_theme);
    ADD ("/appearance/options", test_options);
    ADD ("/appearance/default-shell-features", test_default_shell_features);
    ADD ("/appearance/config-file-include", test_config_file_include);
#undef ADD
    return g_test_run ();
}
