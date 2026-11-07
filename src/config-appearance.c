#include "config-appearance.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define MAX_INCLUDE_DEPTH 10

#define DEFAULT_SHELL_FEATURES \
    (CORRAL_SHELL_FEATURE_CURSOR | CORRAL_SHELL_FEATURE_PATH | CORRAL_SHELL_FEATURE_TITLE)

static const struct {
    const char *name;
    guint bit;
} shell_feature_names[] = {
    { "cursor", CORRAL_SHELL_FEATURE_CURSOR },
    { "path", CORRAL_SHELL_FEATURE_PATH },
    { "ssh-env", CORRAL_SHELL_FEATURE_SSH_ENV },
    { "ssh-terminfo", CORRAL_SHELL_FEATURE_SSH_TERMINFO },
    { "sudo", CORRAL_SHELL_FEATURE_SUDO },
    { "title", CORRAL_SHELL_FEATURE_TITLE },
};

typedef struct {
    char *key;
    char *value;
} Entry;

static void
entry_free (gpointer data)
{
    Entry *entry = data;
    g_free (entry->key);
    g_free (entry->value);
    g_free (entry);
}

void
corral_appearance_init (CorralAppearance *self)
{
    memset (self, 0, sizeof *self);
    self->background = (GhosttyColorRgb) { 0x28, 0x2c, 0x34 };
    self->foreground = (GhosttyColorRgb) { 0xff, 0xff, 0xff };
    ghostty_color_palette_default (self->palette);
    self->minimum_contrast = 1.0;
    self->faint_opacity = 0.5;
    self->background_opacity = 1.0;
    self->cursor_opacity = 1.0;
    self->cursor_style = CORRAL_CURSOR_BLOCK;
    self->cursor_style_blink = -1;
    self->padding_left = 2;
    self->padding_right = 2;
    self->padding_top = 2;
    self->padding_bottom = 2;
    self->scrollback_limit_bytes = 50000000;
    self->scrollback_limit_lines = G_MAXSIZE;
    self->image_storage_limit = 320 * 1000 * 1000;
    self->copy_on_select = CORRAL_COPY_ON_SELECT_PRIMARY;
    self->selection_clear_on_typing = TRUE;
    self->mouse_scroll_precision = 1.0;
    self->mouse_scroll_discrete = 3.0;
    self->link_url = TRUE;
    self->clipboard_read = CORRAL_CLIPBOARD_ASK;
    self->clipboard_write = CORRAL_CLIPBOARD_ALLOW;
    self->clipboard_trim_trailing_spaces = TRUE;
    self->term = g_strdup ("xterm-ghostty");
    self->shell_features = DEFAULT_SHELL_FEATURES;
    self->env = g_ptr_array_new_with_free_func (g_free);
}

void
corral_appearance_clear (CorralAppearance *self)
{
    g_clear_pointer (&self->term, g_free);
    g_clear_pointer (&self->env, g_ptr_array_unref);
}

static gboolean
parse_bool (const char *value, gboolean *out)
{
    if (g_ascii_strcasecmp (value, "true") == 0 || g_strcmp0 (value, "1") == 0) {
        *out = TRUE;
        return TRUE;
    }
    if (g_ascii_strcasecmp (value, "false") == 0 || g_strcmp0 (value, "0") == 0) {
        *out = FALSE;
        return TRUE;
    }
    return FALSE;
}

static gboolean
parse_double (const char *value, double *out)
{
    char *end = NULL;
    errno = 0;
    double v = g_ascii_strtod (value, &end);
    if (errno != 0 || end == value || *end != '\0') {
        return FALSE;
    }
    *out = v;
    return TRUE;
}

static gboolean
parse_size (const char *value, gsize *out)
{
    if (g_ascii_strcasecmp (value, "unlimited") == 0) {
        *out = G_MAXSIZE;
        return TRUE;
    }
    guint64 v = 0;
    if (!g_ascii_string_to_unsigned (value, 10, 0, G_MAXSIZE, &v, NULL)) {
        return FALSE;
    }
    *out = (gsize) v;
    return TRUE;
}

static gboolean
parse_rgb (const char *value, GhosttyColorRgb *out)
{
    return ghostty_color_parse (value, strlen (value), out) == GHOSTTY_SUCCESS;
}

static gboolean
parse_term_color (const char *value, CorralTermColor *out)
{
    if (g_strcmp0 (value, "cell-foreground") == 0) {
        out->kind = CORRAL_TERM_COLOR_CELL_FOREGROUND;
        return TRUE;
    }
    if (g_strcmp0 (value, "cell-background") == 0) {
        out->kind = CORRAL_TERM_COLOR_CELL_BACKGROUND;
        return TRUE;
    }
    if (!parse_rgb (value, &out->rgb)) {
        return FALSE;
    }
    out->kind = CORRAL_TERM_COLOR_RGB;
    return TRUE;
}

static gboolean
parse_padding (const char *value, guint *first, guint *second)
{
    char **parts = g_strsplit (value, ",", 2);
    guint64 a = 0;
    guint64 b = 0;
    gboolean ok = g_ascii_string_to_unsigned (g_strstrip (parts[0]), 10, 0, G_MAXUINT, &a, NULL);
    if (ok && parts[1] != NULL) {
        ok = g_ascii_string_to_unsigned (g_strstrip (parts[1]), 10, 0, G_MAXUINT, &b, NULL);
    } else {
        b = a;
    }
    g_strfreev (parts);
    if (ok) {
        *first = (guint) a;
        *second = (guint) b;
    }
    return ok;
}

static gboolean
parse_adjust (const char *value, double *out, gboolean *percent)
{
    gsize len = strlen (value);
    gboolean pct = len > 0 && value[len - 1] == '%';
    char *number = g_strndup (value, pct ? len - 1 : len);
    gboolean ok = parse_double (number, out);
    g_free (number);
    if (ok) {
        *percent = pct;
    }
    return ok;
}

static gboolean
parse_clipboard_access (const char *value, CorralClipboardAccess *out)
{
    if (g_strcmp0 (value, "allow") == 0) {
        *out = CORRAL_CLIPBOARD_ALLOW;
    } else if (g_strcmp0 (value, "deny") == 0) {
        *out = CORRAL_CLIPBOARD_DENY;
    } else if (g_strcmp0 (value, "ask") == 0) {
        *out = CORRAL_CLIPBOARD_ASK;
    } else {
        return FALSE;
    }
    return TRUE;
}

static gboolean
parse_shell_features (const char *value, guint *out)
{
    guint all = 0;
    for (gsize i = 0; i < G_N_ELEMENTS (shell_feature_names); i++) {
        all |= shell_feature_names[i].bit;
    }
    gboolean b = FALSE;
    if (parse_bool (value, &b)) {
        *out = b ? all : 0;
        return TRUE;
    }

    guint features = DEFAULT_SHELL_FEATURES;
    char **parts = g_strsplit (value, ",", -1);
    gboolean ok = TRUE;
    for (char **p = parts; ok && *p != NULL; p++) {
        const char *part = g_strstrip (*p);
        gboolean on = !g_str_has_prefix (part, "no-");
        if (!on) {
            part += 3;
        }
        gboolean found = FALSE;
        for (gsize i = 0; i < G_N_ELEMENTS (shell_feature_names); i++) {
            if (g_strcmp0 (part, shell_feature_names[i].name) == 0) {
                guint bit = shell_feature_names[i].bit;
                features = on ? (features | bit) : (features & ~bit);
                found = TRUE;
                break;
            }
        }
        ok = found;
    }
    g_strfreev (parts);
    if (ok) {
        *out = features;
    }
    return ok;
}

static void
apply_entry (CorralAppearance *self,
             const CorralAppearance *defaults,
             const char *key,
             const char *value)
{
    gboolean empty = value[0] == '\0';
    gboolean ok = TRUE;

#define RESET(field) self->field = defaults->field

    if (g_strcmp0 (key, "background") == 0) {
        if (empty) RESET (background); else ok = parse_rgb (value, &self->background);
    } else if (g_strcmp0 (key, "foreground") == 0) {
        if (empty) RESET (foreground); else ok = parse_rgb (value, &self->foreground);
    } else if (g_strcmp0 (key, "palette") == 0) {
        if (empty) {
            memcpy (self->palette, defaults->palette, sizeof self->palette);
            memset (&self->palette_set, 0, sizeof self->palette_set);
        } else {
            uint8_t index = 0;
            GhosttyColorRgb rgb;
            ok = ghostty_color_parse_palette_entry (value, strlen (value), &index, &rgb) ==
                 GHOSTTY_SUCCESS;
            if (ok) {
                self->palette[index] = rgb;
                GHOSTTY_COLOR_PALETTE_MASK_SET (&self->palette_set, index);
            }
        }
    } else if (g_strcmp0 (key, "palette-generate") == 0) {
        if (empty) RESET (palette_generate); else ok = parse_bool (value, &self->palette_generate);
    } else if (g_strcmp0 (key, "palette-harmonious") == 0) {
        if (empty) RESET (palette_harmonious); else ok = parse_bool (value, &self->palette_harmonious);
    } else if (g_strcmp0 (key, "cursor-color") == 0) {
        if (empty) RESET (cursor_color); else ok = parse_term_color (value, &self->cursor_color);
    } else if (g_strcmp0 (key, "cursor-text") == 0) {
        if (empty) RESET (cursor_text); else ok = parse_term_color (value, &self->cursor_text);
    } else if (g_strcmp0 (key, "selection-foreground") == 0) {
        if (empty) RESET (selection_foreground);
        else ok = parse_term_color (value, &self->selection_foreground);
    } else if (g_strcmp0 (key, "selection-background") == 0) {
        if (empty) RESET (selection_background);
        else ok = parse_term_color (value, &self->selection_background);
    } else if (g_strcmp0 (key, "bold-color") == 0) {
        if (empty) {
            RESET (bold_color_kind);
        } else if (g_strcmp0 (value, "bright") == 0) {
            self->bold_color_kind = CORRAL_BOLD_COLOR_BRIGHT;
        } else if ((ok = parse_rgb (value, &self->bold_color))) {
            self->bold_color_kind = CORRAL_BOLD_COLOR_RGB;
        }
    } else if (g_strcmp0 (key, "minimum-contrast") == 0) {
        if (empty) RESET (minimum_contrast); else ok = parse_double (value, &self->minimum_contrast);
    } else if (g_strcmp0 (key, "faint-opacity") == 0) {
        if (empty) RESET (faint_opacity); else ok = parse_double (value, &self->faint_opacity);
    } else if (g_strcmp0 (key, "background-opacity") == 0) {
        if (empty) RESET (background_opacity); else ok = parse_double (value, &self->background_opacity);
    } else if (g_strcmp0 (key, "cursor-opacity") == 0) {
        if (empty) RESET (cursor_opacity); else ok = parse_double (value, &self->cursor_opacity);
    } else if (g_strcmp0 (key, "cursor-style") == 0) {
        if (empty) RESET (cursor_style);
        else if (g_strcmp0 (value, "block") == 0) self->cursor_style = CORRAL_CURSOR_BLOCK;
        else if (g_strcmp0 (value, "bar") == 0) self->cursor_style = CORRAL_CURSOR_BAR;
        else if (g_strcmp0 (value, "underline") == 0) self->cursor_style = CORRAL_CURSOR_UNDERLINE;
        else if (g_strcmp0 (value, "block_hollow") == 0) self->cursor_style = CORRAL_CURSOR_BLOCK_HOLLOW;
        else ok = FALSE;
    } else if (g_strcmp0 (key, "cursor-style-blink") == 0) {
        gboolean b = FALSE;
        if (empty) RESET (cursor_style_blink);
        else if ((ok = parse_bool (value, &b))) self->cursor_style_blink = b ? 1 : 0;
    } else if (g_strcmp0 (key, "window-padding-x") == 0) {
        if (empty) {
            RESET (padding_left);
            RESET (padding_right);
        } else {
            ok = parse_padding (value, &self->padding_left, &self->padding_right);
        }
    } else if (g_strcmp0 (key, "window-padding-y") == 0) {
        if (empty) {
            RESET (padding_top);
            RESET (padding_bottom);
        } else {
            ok = parse_padding (value, &self->padding_top, &self->padding_bottom);
        }
    } else if (g_strcmp0 (key, "window-padding-balance") == 0) {
        if (empty) RESET (padding_balance);
        else if (g_strcmp0 (value, "false") == 0) self->padding_balance = CORRAL_PADDING_BALANCE_FALSE;
        else if (g_strcmp0 (value, "true") == 0) self->padding_balance = CORRAL_PADDING_BALANCE_TRUE;
        else if (g_strcmp0 (value, "equal") == 0) self->padding_balance = CORRAL_PADDING_BALANCE_EQUAL;
        else ok = FALSE;
    } else if (g_strcmp0 (key, "window-padding-color") == 0) {
        if (empty) RESET (padding_color);
        else if (g_strcmp0 (value, "background") == 0) self->padding_color = CORRAL_PADDING_COLOR_BACKGROUND;
        else if (g_strcmp0 (value, "extend") == 0) self->padding_color = CORRAL_PADDING_COLOR_EXTEND;
        else if (g_strcmp0 (value, "extend-always") == 0) self->padding_color = CORRAL_PADDING_COLOR_EXTEND_ALWAYS;
        else ok = FALSE;
    } else if (g_strcmp0 (key, "adjust-cell-width") == 0) {
        if (empty) {
            RESET (adjust_cell_width);
            RESET (adjust_cell_width_percent);
        } else {
            ok = parse_adjust (value, &self->adjust_cell_width, &self->adjust_cell_width_percent);
        }
    } else if (g_strcmp0 (key, "adjust-cell-height") == 0) {
        if (empty) {
            RESET (adjust_cell_height);
            RESET (adjust_cell_height_percent);
        } else {
            ok = parse_adjust (value, &self->adjust_cell_height, &self->adjust_cell_height_percent);
        }
    } else if (g_strcmp0 (key, "scrollback-limit-bytes") == 0 ||
               g_strcmp0 (key, "scrollback-limit") == 0) {
        if (empty) RESET (scrollback_limit_bytes);
        else ok = parse_size (value, &self->scrollback_limit_bytes);
    } else if (g_strcmp0 (key, "scrollback-limit-lines") == 0) {
        if (empty) RESET (scrollback_limit_lines);
        else ok = parse_size (value, &self->scrollback_limit_lines);
    } else if (g_strcmp0 (key, "image-storage-limit") == 0) {
        if (empty) RESET (image_storage_limit);
        else ok = parse_size (value, &self->image_storage_limit);
    } else if (g_strcmp0 (key, "copy-on-select") == 0) {
        if (empty) RESET (copy_on_select);
        else if (g_strcmp0 (value, "none") == 0 || g_strcmp0 (value, "false") == 0)
            self->copy_on_select = CORRAL_COPY_ON_SELECT_NONE;
        else if (g_strcmp0 (value, "primary") == 0 || g_strcmp0 (value, "true") == 0)
            self->copy_on_select = CORRAL_COPY_ON_SELECT_PRIMARY;
        else if (g_strcmp0 (value, "clipboard") == 0)
            self->copy_on_select = CORRAL_COPY_ON_SELECT_CLIPBOARD;
        else if (g_strcmp0 (value, "both") == 0)
            self->copy_on_select = CORRAL_COPY_ON_SELECT_BOTH;
        else ok = FALSE;
    } else if (g_strcmp0 (key, "selection-clear-on-typing") == 0) {
        if (empty) RESET (selection_clear_on_typing);
        else ok = parse_bool (value, &self->selection_clear_on_typing);
    } else if (g_strcmp0 (key, "selection-clear-on-copy") == 0) {
        if (empty) RESET (selection_clear_on_copy);
        else ok = parse_bool (value, &self->selection_clear_on_copy);
    } else if (g_strcmp0 (key, "mouse-hide-while-typing") == 0) {
        if (empty) RESET (mouse_hide_while_typing);
        else ok = parse_bool (value, &self->mouse_hide_while_typing);
    } else if (g_strcmp0 (key, "mouse-scroll-multiplier") == 0) {
        if (empty) {
            RESET (mouse_scroll_precision);
            RESET (mouse_scroll_discrete);
        } else {
            double both = 0;
            if (parse_double (value, &both)) {
                self->mouse_scroll_precision = both;
                self->mouse_scroll_discrete = both;
            } else {
                char **parts = g_strsplit (value, ",", -1);
                for (char **p = parts; ok && *p != NULL; p++) {
                    char **kv = g_strsplit (g_strstrip (*p), ":", 2);
                    if (kv[1] == NULL) {
                        ok = FALSE;
                    } else if (g_strcmp0 (g_strstrip (kv[0]), "precision") == 0) {
                        ok = parse_double (g_strstrip (kv[1]), &self->mouse_scroll_precision);
                    } else if (g_strcmp0 (g_strstrip (kv[0]), "discrete") == 0) {
                        ok = parse_double (g_strstrip (kv[1]), &self->mouse_scroll_discrete);
                    } else {
                        ok = FALSE;
                    }
                    g_strfreev (kv);
                }
                g_strfreev (parts);
            }
        }
    } else if (g_strcmp0 (key, "link-url") == 0) {
        if (empty) RESET (link_url); else ok = parse_bool (value, &self->link_url);
    } else if (g_strcmp0 (key, "clipboard-read") == 0) {
        if (empty) RESET (clipboard_read);
        else ok = parse_clipboard_access (value, &self->clipboard_read);
    } else if (g_strcmp0 (key, "clipboard-write") == 0) {
        if (empty) RESET (clipboard_write);
        else ok = parse_clipboard_access (value, &self->clipboard_write);
    } else if (g_strcmp0 (key, "clipboard-trim-trailing-spaces") == 0) {
        if (empty) RESET (clipboard_trim_trailing_spaces);
        else ok = parse_bool (value, &self->clipboard_trim_trailing_spaces);
    } else if (g_strcmp0 (key, "term") == 0) {
        g_free (self->term);
        self->term = g_strdup (empty ? defaults->term : value);
    } else if (g_strcmp0 (key, "shell-integration-features") == 0) {
        if (empty) RESET (shell_features);
        else ok = parse_shell_features (value, &self->shell_features);
    } else if (g_strcmp0 (key, "env") == 0) {
        if (empty) {
            g_ptr_array_set_size (self->env, 0);
        } else if (strchr (value, '=') != NULL && value[0] != '=') {
            g_ptr_array_add (self->env, g_strdup (value));
        } else {
            ok = FALSE;
        }
    }
    /* Everything else, including font and keybind keys, is not ours. */

#undef RESET

    if (!ok) {
        g_message ("ghostty config: ignoring invalid %s = %s", key, value);
    }
}

static char *
unquote (char *value)
{
    gsize len = strlen (value);
    if (len >= 2 && value[0] == '"' && value[len - 1] == '"') {
        value[len - 1] = '\0';
        return value + 1;
    }
    return value;
}

static gboolean
read_entries (GPtrArray *out, const char *path, gboolean optional, int depth)
{
    char *contents = NULL;
    GError *error = NULL;
    if (!g_file_get_contents (path, &contents, NULL, &error)) {
        if (!optional || !g_error_matches (error, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
            g_message ("ghostty config: cannot read %s: %s", path, error->message);
        }
        g_clear_error (&error);
        return FALSE;
    }

    GPtrArray *includes = g_ptr_array_new_with_free_func (g_free);
    char *dir = g_path_get_dirname (path);
    char **lines = g_strsplit (contents, "\n", -1);
    for (char **l = lines; *l != NULL; l++) {
        char *line = g_strstrip (*l);
        if (line[0] == '\0' || line[0] == '#') {
            continue;
        }
        char *eq = strchr (line, '=');
        if (eq == NULL) {
            continue;
        }
        *eq = '\0';
        char *key = g_strstrip (line);
        char *value = unquote (g_strstrip (eq + 1));
        if (g_strcmp0 (key, "config-file") == 0) {
            if (value[0] != '\0') {
                g_ptr_array_add (includes, g_strdup (value));
            }
            continue;
        }
        Entry *entry = g_new0 (Entry, 1);
        entry->key = g_strdup (key);
        entry->value = g_strdup (value);
        g_ptr_array_add (out, entry);
    }
    g_strfreev (lines);
    g_free (contents);

    if (depth < MAX_INCLUDE_DEPTH) {
        for (guint i = 0; i < includes->len; i++) {
            const char *include = g_ptr_array_index (includes, i);
            gboolean include_optional = include[0] == '?';
            if (include_optional) {
                include++;
            }
            char *full = g_path_is_absolute (include)
                ? g_strdup (include)
                : g_build_filename (dir, include, NULL);
            read_entries (out, full, include_optional, depth + 1);
            g_free (full);
        }
    }
    g_free (dir);
    g_ptr_array_unref (includes);
    return TRUE;
}

static char *
pick_theme_name (const char *theme, gboolean prefer_dark)
{
    if (strchr (theme, ':') == NULL || g_path_is_absolute (theme)) {
        return g_strdup (theme);
    }
    char *light = NULL;
    char *dark = NULL;
    char **parts = g_strsplit (theme, ",", -1);
    for (char **p = parts; *p != NULL; p++) {
        char *part = g_strstrip (*p);
        if (g_str_has_prefix (part, "light:")) {
            g_free (light);
            light = g_strdup (g_strstrip (part + 6));
        } else if (g_str_has_prefix (part, "dark:")) {
            g_free (dark);
            dark = g_strdup (g_strstrip (part + 5));
        }
    }
    g_strfreev (parts);
    char *chosen = prefer_dark ? (dark != NULL ? dark : light) : (light != NULL ? light : dark);
    char *result = g_strdup (chosen != NULL ? chosen : theme);
    g_free (light);
    g_free (dark);
    return result;
}

static char *
find_theme (const char *name, const char *const *theme_dirs)
{
    if (g_path_is_absolute (name)) {
        return g_file_test (name, G_FILE_TEST_IS_REGULAR) ? g_strdup (name) : NULL;
    }
    if (strchr (name, G_DIR_SEPARATOR) != NULL) {
        return NULL;
    }
    for (const char *const *dir = theme_dirs; dir != NULL && *dir != NULL; dir++) {
        char *path = g_build_filename (*dir, name, NULL);
        if (g_file_test (path, G_FILE_TEST_IS_REGULAR)) {
            return path;
        }
        g_free (path);
    }
    return NULL;
}

void
corral_appearance_load (CorralAppearance *self,
                        const char *const *files,
                        const char *const *theme_dirs,
                        gboolean prefer_dark)
{
    CorralAppearance defaults;
    corral_appearance_init (&defaults);

    GPtrArray *user = g_ptr_array_new_with_free_func (entry_free);
    for (const char *const *f = files; f != NULL && *f != NULL; f++) {
        read_entries (user, *f, TRUE, 0);
    }

    const char *theme = NULL;
    for (guint i = 0; i < user->len; i++) {
        Entry *entry = g_ptr_array_index (user, i);
        if (g_strcmp0 (entry->key, "theme") == 0) {
            theme = entry->value[0] != '\0' ? entry->value : NULL;
        }
    }

    if (theme != NULL) {
        char *name = pick_theme_name (theme, prefer_dark);
        char *path = find_theme (name, theme_dirs);
        if (path == NULL) {
            g_message ("ghostty config: theme \"%s\" not found", name);
        } else {
            GPtrArray *themed = g_ptr_array_new_with_free_func (entry_free);
            read_entries (themed, path, FALSE, MAX_INCLUDE_DEPTH);
            for (guint i = 0; i < themed->len; i++) {
                Entry *entry = g_ptr_array_index (themed, i);
                if (g_strcmp0 (entry->key, "theme") != 0) {
                    apply_entry (self, &defaults, entry->key, entry->value);
                }
            }
            g_ptr_array_unref (themed);
        }
        g_free (path);
        g_free (name);
    }

    for (guint i = 0; i < user->len; i++) {
        Entry *entry = g_ptr_array_index (user, i);
        apply_entry (self, &defaults, entry->key, entry->value);
    }
    g_ptr_array_unref (user);

    if (self->palette_generate) {
        ghostty_color_palette_generate (self->palette, &self->palette_set,
                                        &self->background, &self->foreground,
                                        self->palette_harmonious, self->palette);
    }
    corral_appearance_clear (&defaults);
}

char **
corral_appearance_default_files (void)
{
    GStrvBuilder *builder = g_strv_builder_new ();
    const char *config = g_get_user_config_dir ();
    g_strv_builder_take (builder, g_build_filename (config, "ghostty", "config", NULL));
    g_strv_builder_take (builder, g_build_filename (config, "ghostty", "config.ghostty", NULL));
    char **files = g_strv_builder_end (builder);
    g_strv_builder_unref (builder);
    return files;
}

char **
corral_appearance_default_theme_dirs (const char *bundled_dir)
{
    GStrvBuilder *builder = g_strv_builder_new ();
    g_strv_builder_take (builder,
                         g_build_filename (g_get_user_config_dir (), "ghostty", "themes", NULL));
    const char *resources = g_getenv ("GHOSTTY_RESOURCES_DIR");
    if (resources != NULL && resources[0] != '\0') {
        char *dir = g_build_filename (resources, "themes", NULL);
        if (g_file_test (dir, G_FILE_TEST_IS_DIR)) {
            g_strv_builder_take (builder, dir);
        } else {
            g_free (dir);
        }
    }
    if (bundled_dir != NULL) {
        g_strv_builder_add (builder, bundled_dir);
    }
    char **dirs = g_strv_builder_end (builder);
    g_strv_builder_unref (builder);
    return dirs;
}

char *
corral_appearance_shell_features (const CorralAppearance *self)
{
    GString *out = g_string_new (NULL);
    for (gsize i = 0; i < G_N_ELEMENTS (shell_feature_names); i++) {
        if ((self->shell_features & shell_feature_names[i].bit) == 0) {
            continue;
        }
        if (out->len > 0) {
            g_string_append_c (out, ',');
        }
        g_string_append (out, shell_feature_names[i].name);
        if (shell_feature_names[i].bit == CORRAL_SHELL_FEATURE_CURSOR) {
            g_string_append (out, self->cursor_style_blink == 0 ? ":steady" : ":blink");
        }
    }
    if (out->len == 0) {
        g_string_free (out, TRUE);
        return NULL;
    }
    return g_string_free (out, FALSE);
}
