#pragma once

#include <ghostty/vt.h>
#include <glib.h>

G_BEGIN_DECLS

typedef enum {
    CORRAL_TERM_COLOR_UNSET,
    CORRAL_TERM_COLOR_RGB,
    CORRAL_TERM_COLOR_CELL_FOREGROUND,
    CORRAL_TERM_COLOR_CELL_BACKGROUND,
} CorralTermColorKind;

typedef struct {
    CorralTermColorKind kind;
    GhosttyColorRgb rgb;
} CorralTermColor;

typedef enum {
    CORRAL_BOLD_COLOR_UNSET,
    CORRAL_BOLD_COLOR_RGB,
    CORRAL_BOLD_COLOR_BRIGHT,
} CorralBoldColorKind;

typedef enum {
    CORRAL_CURSOR_BLOCK,
    CORRAL_CURSOR_BAR,
    CORRAL_CURSOR_UNDERLINE,
    CORRAL_CURSOR_BLOCK_HOLLOW,
} CorralCursorStyle;

typedef enum {
    CORRAL_PADDING_BALANCE_FALSE,
    CORRAL_PADDING_BALANCE_TRUE,
    CORRAL_PADDING_BALANCE_EQUAL,
} CorralPaddingBalance;

typedef enum {
    CORRAL_PADDING_COLOR_BACKGROUND,
    CORRAL_PADDING_COLOR_EXTEND,
    CORRAL_PADDING_COLOR_EXTEND_ALWAYS,
} CorralPaddingColor;

typedef enum {
    CORRAL_COPY_ON_SELECT_NONE,
    CORRAL_COPY_ON_SELECT_PRIMARY,
    CORRAL_COPY_ON_SELECT_CLIPBOARD,
    CORRAL_COPY_ON_SELECT_BOTH,
} CorralCopyOnSelect;

typedef enum {
    CORRAL_CLIPBOARD_ALLOW,
    CORRAL_CLIPBOARD_DENY,
    CORRAL_CLIPBOARD_ASK,
} CorralClipboardAccess;

/* Shell-integration feature bits, in GHOSTTY_SHELL_FEATURES order. */
enum {
    CORRAL_SHELL_FEATURE_CURSOR = 1 << 0,
    CORRAL_SHELL_FEATURE_PATH = 1 << 1,
    CORRAL_SHELL_FEATURE_SSH_ENV = 1 << 2,
    CORRAL_SHELL_FEATURE_SSH_TERMINFO = 1 << 3,
    CORRAL_SHELL_FEATURE_SUDO = 1 << 4,
    CORRAL_SHELL_FEATURE_TITLE = 1 << 5,
};

/* The Ghostty 1.4 options that change an embedded terminal. Font family,
 * font size, and keybinds stay on Corral's settings and are not here. */
typedef struct {
    GhosttyColorRgb background;
    GhosttyColorRgb foreground;
    GhosttyColorRgb palette[256];
    GhosttyColorPaletteMask palette_set;
    gboolean palette_generate;
    gboolean palette_harmonious;

    CorralTermColor cursor_color;
    CorralTermColor cursor_text;
    CorralTermColor selection_foreground;
    CorralTermColor selection_background;
    CorralBoldColorKind bold_color_kind;
    GhosttyColorRgb bold_color;
    double minimum_contrast;
    double faint_opacity;
    double background_opacity;
    double cursor_opacity;

    CorralCursorStyle cursor_style;
    /* -1 unset, 0 steady, 1 blink. Unset follows DECSCUSR and blinks. */
    int cursor_style_blink;

    guint padding_left;
    guint padding_right;
    guint padding_top;
    guint padding_bottom;
    CorralPaddingBalance padding_balance;
    CorralPaddingColor padding_color;
    /* Unset is 0. Percent values carry percent = TRUE. */
    double adjust_cell_width;
    gboolean adjust_cell_width_percent;
    double adjust_cell_height;
    gboolean adjust_cell_height_percent;

    gsize scrollback_limit_bytes;
    gsize scrollback_limit_lines;
    gsize image_storage_limit;

    CorralCopyOnSelect copy_on_select;
    gboolean selection_clear_on_typing;
    gboolean selection_clear_on_copy;
    gboolean mouse_hide_while_typing;
    double mouse_scroll_precision;
    double mouse_scroll_discrete;
    gboolean link_url;
    CorralClipboardAccess clipboard_read;
    CorralClipboardAccess clipboard_write;
    gboolean clipboard_trim_trailing_spaces;

    char *term;
    guint shell_features;
    /* KEY=VALUE pairs from the `env` option, in order. */
    GPtrArray *env;
} CorralAppearance;

void corral_appearance_init (CorralAppearance *self);
void corral_appearance_clear (CorralAppearance *self);

/* Load FILES in order on top of the defaults. A file that does not exist
 * is skipped. `config-file` includes are followed. `theme` is resolved in
 * THEME_DIRS, first match wins, and the theme is applied under the files. */
void corral_appearance_load (CorralAppearance *self,
                             const char *const *files,
                             const char *const *theme_dirs,
                             gboolean prefer_dark);

/* The files Ghostty 1.4 loads by default on Linux. */
char **corral_appearance_default_files (void);

/* Ghostty's theme search order, then BUNDLED_DIR when it is not NULL. */
char **corral_appearance_default_theme_dirs (const char *bundled_dir);

/* GHOSTTY_SHELL_FEATURES for the child, as Ghostty 1.4 writes it. */
char *corral_appearance_shell_features (const CorralAppearance *self);

G_END_DECLS
