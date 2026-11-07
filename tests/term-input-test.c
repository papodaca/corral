#define _GNU_SOURCE

#include "term-session.h"

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <string.h>

#define XKB_A 0x0026
#define XKB_C 0x0036
#define XKB_V 0x0037
#define CELL_W 8
#define CELL_H 16
#define CELL_X(col) ((col) * CELL_W + CELL_W / 2.0)
#define CELL_Y(row) ((row) * CELL_H + CELL_H / 2.0)

typedef enum {
    READ_NOW,
    READ_IDLE,
    READ_HELD,
    READ_NEVER,
} ReadMode;

typedef struct {
    CorralTermSession *session;
    char *dir;
    char *out;
    gboolean exited;

    GPtrArray *writes;
    char *clipboard;
    ReadMode read_mode;
    guint reads;
    CorralTermTextFn held_done;
    gpointer held_data;
    char *opened;
    gboolean (*during_read) (gpointer fixture);
    guint clipboard_timeout_ms;
} Fixture;

static void
host_write (gpointer host, CorralTermClipboard which, const char *text)
{
    Fixture *f = host;

    g_ptr_array_add (f->writes, g_strdup_printf ("%s:%s", which == CORRAL_TERM_CLIPBOARD_PRIMARY ? "primary"
                                                                                                 : "standard",
                                                text));
}

typedef struct {
    CorralTermTextFn done;
    gpointer data;
    char *text;
} IdleReply;

static gboolean
reply_idle (gpointer data)
{
    IdleReply *reply = data;

    reply->done (reply->text, reply->data);
    g_free (reply->text);
    g_free (reply);
    return G_SOURCE_REMOVE;
}

static void
host_read (gpointer host, CorralTermClipboard which, CorralTermTextFn done, gpointer data)
{
    Fixture *f = host;
    IdleReply *reply;

    f->reads++;
    switch (f->read_mode) {
    case READ_NOW:
        done (f->clipboard, data);
        break;
    case READ_IDLE:
        reply = g_new0 (IdleReply, 1);
        *reply = (IdleReply) { done, data, g_strdup (f->clipboard) };
        g_idle_add (reply_idle, reply);
        break;
    case READ_HELD:
        f->held_done = done;
        f->held_data = data;
        if (f->during_read)
            g_idle_add (f->during_read, f);
        break;
    case READ_NEVER:
        /* Answer long after any timeout, as a stuck clipboard would. */
        reply = g_new0 (IdleReply, 1);
        *reply = (IdleReply) { done, data, g_strdup ("late") };
        g_timeout_add (3000, reply_idle, reply);
        break;
    }
}

static void
host_open (gpointer host, const char *url)
{
    Fixture *f = host;

    g_free (f->opened);
    f->opened = g_strdup (url);
}

static const CorralTermHost host = {
    .write_text = host_write,
    .read_text = host_read,
    .open_url = host_open,
};

static gboolean
time_up (gpointer data)
{
    *(gboolean *) data = TRUE;
    return G_SOURCE_REMOVE;
}

static void
spin (guint ms)
{
    gboolean done = FALSE;

    g_timeout_add (ms, time_up, &done);
    while (!done)
        g_main_context_iteration (NULL, TRUE);
}

static char *
screen_text (CorralTermSession *session)
{
    GhosttyFormatter formatter = NULL;
    uint8_t *buf = NULL;
    size_t len = 0;
    char *text;

    ghostty_formatter_terminal_new (NULL, &formatter, corral_term_session_get_terminal (session));
    ghostty_formatter_format_alloc (formatter, NULL, &buf, &len);
    text = buf ? g_strndup ((const char *) buf, len) : g_strdup ("");
    ghostty_free (NULL, buf, len);
    ghostty_formatter_free (formatter);
    return text;
}

static void
on_exited (CorralTermSession *session, int status, Fixture *f)
{
    f->exited = TRUE;
}

/* Start a child that prints SETUP and then copies everything it reads into
 * a file, so the test sees the exact bytes the session wrote to the pty.
 * BEFORE_CAT runs in the child between the two. */
static void
fixture_start (Fixture *f, const char *setup, const char *before_cat)
{
    g_autofree char *script = g_strdup_printf ("stty raw -echo; printf '%%s' \"$2\"; %s exec cat > \"$1\"",
                                               before_cat ? before_cat : "");
    g_autoptr (GError) error = NULL;
    const char *argv[] = { "sh", "-c", script, "sh", NULL, setup, NULL };

    f->writes = g_ptr_array_new_with_free_func (g_free);
    f->dir = g_dir_make_tmp ("corral-term-input-XXXXXX", NULL);
    f->out = g_build_filename (f->dir, "in", NULL);
    argv[4] = f->out;
    f->session = corral_term_session_new (NULL, 80, 24, CELL_W, CELL_H);
    corral_term_session_set_host (f->session, &host, f);
    if (f->clipboard_timeout_ms)
        corral_term_session_set_clipboard_timeout (f->session, f->clipboard_timeout_ms);
    g_signal_connect (f->session, "exited", G_CALLBACK (on_exited), f);
    g_assert_true (corral_term_session_spawn (f->session, argv, f->dir, &error));
    g_assert_no_error (error);

    for (int i = 0; i < 500 && f->session; i++) {
        g_autofree char *text = screen_text (f->session);
        if (strstr (text, "READY") && g_file_test (f->out, G_FILE_TEST_EXISTS))
            return;
        spin (10);
    }
    if (f->session)
        g_error ("child never printed READY");
}

static void
fixture_stop (Fixture *f)
{
    g_clear_object (&f->session);
    g_unlink (f->out);
    g_rmdir (f->dir);
    g_free (f->out);
    g_free (f->dir);
    g_ptr_array_unref (f->writes);
    g_free (f->clipboard);
    g_free (f->opened);
}

static char *
child_input (Fixture *f)
{
    char *contents = NULL;

    g_file_get_contents (f->out, &contents, NULL, NULL);
    return contents ? contents : g_strdup ("");
}

/* Wait until the child has read at least LEN bytes, or a while. */
static char *
wait_input (Fixture *f, gsize len)
{
    for (int i = 0; i < 300; i++) {
        char *got = child_input (f);
        if (strlen (got) >= len)
            return got;
        g_free (got);
        spin (10);
    }
    return child_input (f);
}

static void
key (Fixture *f, guint keycode, GhosttyMods mods, gunichar unshifted, const char *text, gboolean expect)
{
    CorralTermKey press = {
        .action = GHOSTTY_KEY_ACTION_PRESS,
        .keycode = keycode,
        .mods = mods,
        .unshifted_codepoint = unshifted,
        .text = text,
    };
    CorralTermKey release = press;

    release.action = GHOSTTY_KEY_ACTION_RELEASE;
    release.text = NULL;
    g_assert_cmpint (corral_term_session_key (f->session, &press), ==, expect);
    corral_term_session_key (f->session, &release);
}

static void
drag (Fixture *f, int from_col, int to_col, int row, GhosttyMods mods, gint64 time_us)
{
    corral_term_session_mouse_motion (f->session, CELL_X (from_col), CELL_Y (row), mods);
    corral_term_session_mouse_button (f->session, TRUE, 1, CELL_X (from_col), CELL_Y (row), mods, time_us);
    /* A drag takes in the last cell once the pointer passes its middle. */
    corral_term_session_mouse_motion (f->session, CELL_X (to_col) + 2, CELL_Y (row), mods);
    corral_term_session_mouse_button (f->session, FALSE, 1, CELL_X (to_col) + 2, CELL_Y (row), mods,
                                      time_us + 50000);
}

static void
assert_writes (Fixture *f, const char *const *expected)
{
    guint n = expected ? g_strv_length ((char **) expected) : 0;

    g_assert_cmpuint (f->writes->len, ==, n);
    for (guint i = 0; i < n; i++)
        g_assert_cmpstr (g_ptr_array_index (f->writes, i), ==, expected[i]);
}

static void
test_copy_chord (void)
{
    Fixture f = { 0 };
    g_autofree char *input = NULL;

    fixture_start (&f, "hello world\r\nREADY", NULL);
    drag (&f, 0, 4, 0, 0, 1000000);
    key (&f, XKB_C, GHOSTTY_MODS_CTRL | GHOSTTY_MODS_SHIFT, 'c', "C", TRUE);
    spin (100);

    input = child_input (&f);
    g_assert_cmpstr (input, ==, "");
    assert_writes (&f, (const char *[]) { "primary:hello", "standard:hello", NULL });
    fixture_stop (&f);
}

static void
test_plain_and_ctrl_letters (void)
{
    Fixture f = { 0 };
    g_autofree char *input = NULL;

    fixture_start (&f, "READY", NULL);
    key (&f, XKB_A, 0, 'a', "a", TRUE);
    key (&f, XKB_A, GHOSTTY_MODS_CTRL, 'a', "a", TRUE);
    input = wait_input (&f, 2);

    g_assert_cmpstr (input, ==, "a\001");
    assert_writes (&f, NULL);
    fixture_stop (&f);
}

static void
test_drag_without_tracking (void)
{
    Fixture f = { 0 };
    g_autofree char *input = NULL;

    fixture_start (&f, "hello world\r\nREADY", NULL);
    drag (&f, 6, 10, 0, 0, 1000000);
    spin (100);

    input = child_input (&f);
    g_assert_cmpstr (input, ==, "");
    assert_writes (&f, (const char *[]) { "primary:world", NULL });
    fixture_stop (&f);
}

static void
test_drag_with_tracking (void)
{
    Fixture f = { 0 };
    g_autofree char *input = NULL;

    fixture_start (&f, "\033[?1000h\033[?1006hhello world\r\nREADY", NULL);
    drag (&f, 0, 4, 0, 0, 1000000);
    input = wait_input (&f, 18);

    g_assert_cmpstr (input, ==, "\033[<0;1;1M\033[<0;5;1m");
    assert_writes (&f, NULL);
    fixture_stop (&f);
}

static void
test_shift_drag_with_tracking (void)
{
    Fixture f = { 0 };
    g_autofree char *input = NULL;

    fixture_start (&f, "\033[?1000h\033[?1006hhello world\r\nREADY", NULL);
    drag (&f, 0, 4, 0, GHOSTTY_MODS_SHIFT, 1000000);
    spin (100);

    input = child_input (&f);
    g_assert_cmpstr (input, ==, "");
    assert_writes (&f, (const char *[]) { "primary:hello", NULL });
    fixture_stop (&f);
}

static void
test_double_click_selects_word (void)
{
    Fixture f = { 0 };

    fixture_start (&f, "hello world\r\nREADY", NULL);
    for (int i = 0; i < 2; i++) {
        gint64 t = 1000000 + i * 100000;
        corral_term_session_mouse_button (f.session, TRUE, 1, CELL_X (8), CELL_Y (0), 0, t);
        corral_term_session_mouse_button (f.session, FALSE, 1, CELL_X (8), CELL_Y (0), 0, t + 20000);
    }

    assert_writes (&f, (const char *[]) { "primary:world", NULL });
    fixture_stop (&f);
}

static void
test_wheel_notch_matches_embed (void)
{
    Fixture f = { 0 };
    g_autofree char *fixture = NULL;
    g_autofree char *input = NULL;
    g_autoptr (GString) want = g_string_new (NULL);
    g_auto (GStrv) lines = NULL;

    g_assert_true (g_file_get_contents (FIXTURES_DIR "/embed-wheel-notch.txt", &fixture, NULL, NULL));
    lines = g_strsplit (fixture, "\n", -1);
    for (char **line = lines; *line; line++) {
        g_autoptr (GString) report = NULL;
        if (**line == '#' || **line == '\0')
            continue;
        report = g_string_new (*line);
        g_string_replace (report, "\\e", "\033", 0);
        g_string_append (want, report->str);
    }

    fixture_start (&f, "\033[?1000h\033[?1006hREADY", NULL);
    corral_term_session_mouse_motion (f.session, CELL_X (40), CELL_Y (0), 0);
    corral_term_session_scroll (f.session, 0, -1, FALSE, 0);
    input = wait_input (&f, want->len);

    g_assert_cmpstr (input, ==, want->str);
    fixture_stop (&f);
}

static void
test_ctrl_click_opens_url (void)
{
    Fixture f = { 0 };

    fixture_start (&f, "see https://example.com/x ok\r\nREADY", NULL);
    corral_term_session_mouse_button (f.session, TRUE, 1, CELL_X (10), CELL_Y (0), GHOSTTY_MODS_CTRL, 1000000);
    corral_term_session_mouse_button (f.session, FALSE, 1, CELL_X (10), CELL_Y (0), GHOSTTY_MODS_CTRL, 1050000);

    g_assert_cmpstr (f.opened, ==, "https://example.com/x");
    fixture_stop (&f);
}

static gboolean
resize_and_type_during_read (gpointer data)
{
    Fixture *f = data;
    GhosttyTerminal terminal = corral_term_session_get_terminal (f->session);
    CorralTermKey x = {
        .action = GHOSTTY_KEY_ACTION_PRESS,
        .keycode = 0x0035,
        .unshifted_codepoint = 'x',
        .text = "x",
    };
    guint16 cols = 0;

    g_assert_true (corral_term_session_is_busy (f->session));
    corral_term_session_resize (f->session, 100, 30, CELL_W, CELL_H);
    corral_term_session_key (f->session, &x);
    ghostty_terminal_get (terminal, GHOSTTY_TERMINAL_DATA_COLS, &cols);
    g_assert_cmpuint (cols, ==, 80);
    f->held_done ("clip", f->held_data);
    return G_SOURCE_REMOVE;
}

static void
test_osc52_read_defers_resize_and_input (void)
{
    Fixture f = { .read_mode = READ_HELD, .during_read = resize_and_type_during_read };
    g_autofree char *input = NULL;
    guint16 cols = 0;
    const char *want = "\033]52;c;Y2xpcA==\007x";

    fixture_start (&f, "READY", "printf '\\033]52;c;?\\007';");
    input = wait_input (&f, strlen (want));
    ghostty_terminal_get (corral_term_session_get_terminal (f.session), GHOSTTY_TERMINAL_DATA_COLS, &cols);

    g_assert_cmpuint (f.reads, ==, 1);
    g_assert_cmpstr (input, ==, want);
    g_assert_cmpuint (cols, ==, 100);
    fixture_stop (&f);
}

static gboolean
drop_session_during_read (gpointer data)
{
    Fixture *f = data;
    CorralTermTextFn done = f->held_done;
    gpointer done_data = f->held_data;

    g_clear_object (&f->session);
    done ("clip", done_data);
    return G_SOURCE_REMOVE;
}

static void
test_osc52_read_holds_session (void)
{
    Fixture f = { .read_mode = READ_HELD, .during_read = drop_session_during_read };
    GWeakRef weak;
    gboolean gone = FALSE;

    fixture_start (&f, "READY", "printf '\\033]52;c;?\\007';");
    g_weak_ref_init (&weak, f.session);
    for (int i = 0; i < 300 && !gone; i++) {
        g_autoptr (CorralTermSession) alive = NULL;
        spin (10);
        alive = g_weak_ref_get (&weak);
        gone = f.reads == 1 && alive == NULL;
    }
    g_weak_ref_clear (&weak);
    g_assert_true (gone);
    fixture_stop (&f);
}

static void
test_osc52_read_timeout (void)
{
    Fixture f = { .read_mode = READ_NEVER, .clipboard_timeout_ms = 200 };
    g_autofree char *input = NULL;
    const char *want = "\033]52;c;\007";

    g_test_expect_message ("corral", G_LOG_LEVEL_WARNING, "clipboard read for OSC 52 timed out after 200 ms");
    fixture_start (&f, "READY", "printf '\\033]52;c;?\\007';");
    input = wait_input (&f, strlen (want));
    g_test_assert_expected_messages ();

    g_assert_cmpstr (input, ==, want);
    fixture_stop (&f);
}

static void
test_large_paste (void)
{
    Fixture f = { 0 };
    gsize size = 1024 * 1024;
    g_autofree char *text = g_malloc (size + 1);
    g_autofree char *input = NULL;

    memset (text, 'p', size);
    text[size] = '\0';
    fixture_start (&f, "READY", "sleep 0.5;");
    corral_term_session_paste (f.session, CORRAL_TERM_CLIPBOARD_STANDARD, text);
    g_assert_cmpuint (corral_term_session_get_pending_output (f.session), >, 0);

    for (int i = 0; i < 1000; i++) {
        g_free (input);
        input = child_input (&f);
        if (strlen (input) >= size)
            break;
        spin (10);
    }
    g_assert_cmpuint (strlen (input), ==, size);
    g_assert_true (strspn (input, "p") == size);
    fixture_stop (&f);
}

static void
test_paste_chord_brackets (void)
{
    Fixture f = { .read_mode = READ_IDLE, .clipboard = g_strdup ("hello") };
    g_autofree char *input = NULL;
    const char *want = "\033[200~hello\033[201~";

    fixture_start (&f, "\033[?2004hREADY", NULL);
    key (&f, XKB_V, GHOSTTY_MODS_CTRL | GHOSTTY_MODS_SHIFT, 'v', "V", TRUE);
    g_assert_cmpstr (child_input (&f), ==, "");
    input = wait_input (&f, strlen (want));

    g_assert_cmpuint (f.reads, ==, 1);
    g_assert_cmpstr (input, ==, want);
    fixture_stop (&f);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/term-input/copy-chord", test_copy_chord);
    g_test_add_func ("/term-input/plain-and-ctrl-letters", test_plain_and_ctrl_letters);
    g_test_add_func ("/term-input/drag-without-tracking", test_drag_without_tracking);
    g_test_add_func ("/term-input/drag-with-tracking", test_drag_with_tracking);
    g_test_add_func ("/term-input/shift-drag-with-tracking", test_shift_drag_with_tracking);
    g_test_add_func ("/term-input/double-click-selects-word", test_double_click_selects_word);
    g_test_add_func ("/term-input/wheel-notch-matches-embed", test_wheel_notch_matches_embed);
    g_test_add_func ("/term-input/ctrl-click-opens-url", test_ctrl_click_opens_url);
    g_test_add_func ("/term-input/osc52-read-defers-resize-and-input", test_osc52_read_defers_resize_and_input);
    g_test_add_func ("/term-input/osc52-read-holds-session", test_osc52_read_holds_session);
    g_test_add_func ("/term-input/osc52-read-timeout", test_osc52_read_timeout);
    g_test_add_func ("/term-input/large-paste", test_large_paste);
    g_test_add_func ("/term-input/paste-chord-brackets", test_paste_chord_brackets);

    return g_test_run ();
}
