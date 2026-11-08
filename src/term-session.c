#define _GNU_SOURCE

#include "term-session.h"
#include "term-keycodes.h"

#include <errno.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <glib-unix.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#define READ_CHUNK (64 * 1024)
/* What a 1.4.0 embed told the child it was, so shell integration and
 * herdr keep their Ghostty paths. */
#define EMBED_VERSION "1.4.0"
#define EXITED_LINE "\r\nProcess exited. Press any key to close the terminal.\033[?25l"
#define CLIPBOARD_TIMEOUT_MS 2000
/* GTK's default gtk-double-click-time. */
#define REPEAT_CLICK_NS (400 * G_GUINT64_CONSTANT (1000000))
#define AUTOSCROLL_MS 15
#define MOD_KEYS (GHOSTTY_MODS_SHIFT | GHOSTTY_MODS_CTRL | GHOSTTY_MODS_ALT | GHOSTTY_MODS_SUPER)

typedef enum {
    INPUT_KEY,
    INPUT_BUTTON,
    INPUT_MOTION,
    INPUT_SCROLL,
    INPUT_FOCUS,
    INPUT_PASTE,
} InputKind;

/* One user input, kept whole so it can wait out a libghostty-vt write. */
typedef struct {
    InputKind kind;
    CorralTermKey key;
    gboolean pressed;
    guint button;
    double x;
    double y;
    double dx;
    double dy;
    gboolean precise;
    GhosttyMods mods;
    gint64 time_us;
    CorralTermClipboard clipboard;
    char *text;
} Input;

struct _CorralTermSession {
    GObject parent_instance;

    CorralAppearance appearance;
    GhosttyTerminal terminal;
    guint16 cols;
    guint16 rows;
    guint32 cell_width;
    guint32 cell_height;
    guint padding_left;
    guint padding_top;
    guint padding_right;
    guint padding_bottom;
    gboolean prefer_dark;
    gboolean render_held;
    gboolean in_vt_write;

    int master;
    GPid pid;
    gboolean child_done;
    guint read_source;
    guint write_source;
    guint child_watch;
    GByteArray *outbound;

    const CorralTermHost *host;
    gpointer host_data;
    guint clipboard_timeout_ms;

    GQueue deferred;
    gboolean resize_deferred;
    gboolean close_deferred;
    gboolean exit_deferred;
    int exit_status;

    GhosttyKeyEncoder key_encoder;
    GhosttyKeyEvent key_event;
    GhosttyMouseEncoder mouse_encoder;
    GhosttyMouseEvent mouse_event;
    GhosttySelectionGesture gesture;
    guint buttons_down;
    double mouse_x;
    double mouse_y;
    GhosttyMods mouse_mods;
    gint64 last_click_us;
    guint autoscroll_source;
    double pending_scroll_x;
    double pending_scroll_y;
};

enum {
    SIGNAL_CHANGED,
    SIGNAL_TITLE_CHANGED,
    SIGNAL_BELL,
    SIGNAL_EXITED,
    N_SIGNALS,
};

static guint signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE (CorralTermSession, corral_term_session, G_TYPE_OBJECT)

static gboolean handle_input (CorralTermSession *self, const Input *input);
static void apply_resize (CorralTermSession *self);
static void finish_exit (CorralTermSession *self, int status);

static void
input_free (Input *input)
{
    g_free ((char *) input->key.text);
    g_free (input->text);
    g_free (input);
}

/* Run what arrived while libghostty-vt was mid-write. */
static void
run_deferred (CorralTermSession *self)
{
    Input *input;

    while (!self->in_vt_write && (input = g_queue_pop_head (&self->deferred))) {
        handle_input (self, input);
        input_free (input);
    }
    if (self->in_vt_write)
        return;
    if (self->resize_deferred) {
        self->resize_deferred = FALSE;
        apply_resize (self);
    }
    if (self->exit_deferred) {
        self->exit_deferred = FALSE;
        finish_exit (self, self->exit_status);
    }
    if (self->close_deferred) {
        self->close_deferred = FALSE;
        corral_term_session_close (self);
    }
}

static void
feed (CorralTermSession *self, const guint8 *data, gsize len)
{
    g_return_if_fail (!self->in_vt_write);

    /* A clipboard callback can spin the main loop, and whatever runs
     * there must not free the terminal under this write. */
    g_object_ref (self);
    self->in_vt_write = TRUE;
    ghostty_terminal_vt_write (self->terminal, data, len);
    self->in_vt_write = FALSE;
    run_deferred (self);
    g_object_unref (self);
}

/* Returns TRUE when INPUT was queued to run after the current write. */
static gboolean
defer_input (CorralTermSession *self, const Input *input)
{
    Input *copy;

    if (!self->in_vt_write)
        return FALSE;
    copy = g_memdup2 (input, sizeof *input);
    copy->key.text = g_strdup (input->key.text);
    copy->text = g_strdup (input->text);
    g_queue_push_tail (&self->deferred, copy);
    return TRUE;
}

static gboolean
on_writable (int fd, GIOCondition condition, gpointer user_data)
{
    CorralTermSession *self = user_data;

    while (self->outbound->len > 0) {
        ssize_t n = write (fd, self->outbound->data, self->outbound->len);
        if (n > 0) {
            g_byte_array_remove_range (self->outbound, 0, (guint) n);
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && errno == EAGAIN)
            return G_SOURCE_CONTINUE;
        g_byte_array_set_size (self->outbound, 0);
        break;
    }
    self->write_source = 0;
    return G_SOURCE_REMOVE;
}

static void
queue_output (CorralTermSession *self, const guint8 *data, gsize len)
{
    if (self->master < 0 || len == 0)
        return;
    g_byte_array_append (self->outbound, data, (guint) len);
    if (self->write_source == 0)
        self->write_source = g_unix_fd_add (self->master, G_IO_OUT, on_writable, self);
}

static void
on_write_pty (GhosttyTerminal terminal, void *userdata, const uint8_t *data, size_t len)
{
    queue_output (userdata, data, len);
}

static void
on_title_changed (GhosttyTerminal terminal, void *userdata)
{
    CorralTermSession *self = userdata;
    GhosttyString title = { 0 };

    if (ghostty_terminal_get (terminal, GHOSTTY_TERMINAL_DATA_TITLE, &title) != GHOSTTY_SUCCESS)
        return;
    g_autofree char *text = g_strndup ((const char *) title.ptr, title.len);
    g_signal_emit (self, signals[SIGNAL_TITLE_CHANGED], 0, text);
}

static void
on_bell (GhosttyTerminal terminal, void *userdata)
{
    g_signal_emit (userdata, signals[SIGNAL_BELL], 0);
}

static bool
on_size (GhosttyTerminal terminal, void *userdata, GhosttySizeReportSize *out)
{
    CorralTermSession *self = userdata;

    out->rows = self->rows;
    out->columns = self->cols;
    out->cell_width = self->cell_width;
    out->cell_height = self->cell_height;
    return true;
}

static GhosttyString
on_xtversion (GhosttyTerminal terminal, void *userdata)
{
    return (GhosttyString) { (const uint8_t *) "ghostty " EMBED_VERSION,
                             strlen ("ghostty " EMBED_VERSION) };
}

static bool
on_color_scheme (GhosttyTerminal terminal, void *userdata, GhosttyColorScheme *out)
{
    CorralTermSession *self = userdata;

    *out = self->prefer_dark ? GHOSTTY_COLOR_SCHEME_DARK : GHOSTTY_COLOR_SCHEME_LIGHT;
    return true;
}

static void
on_render_hold (GhosttyTerminal terminal, void *userdata, bool held)
{
    CorralTermSession *self = userdata;

    self->render_held = held;
}

static CorralTermClipboard
clipboard_for (GhosttyClipboardLocation location)
{
    return location == GHOSTTY_CLIPBOARD_LOCATION_STANDARD ? CORRAL_TERM_CLIPBOARD_STANDARD
                                                           : CORRAL_TERM_CLIPBOARD_PRIMARY;
}

static gboolean
is_plain_text (GhosttyString mime)
{
    return mime.len >= 10 && memcmp (mime.ptr, "text/plain", 10) == 0;
}

static void
on_clipboard_write (GhosttyTerminal terminal, void *userdata, const GhosttyClipboardWrite *write)
{
    CorralTermSession *self = userdata;
    GhosttyClipboardWriteReply reply = GHOSTTY_INIT_SIZED (GhosttyClipboardWriteReply);
    const GhosttyClipboardContent *content = NULL;

    if (self->appearance.clipboard_write == CORRAL_CLIPBOARD_DENY || !self->host || !self->host->write_text) {
        reply.result = GHOSTTY_CLIPBOARD_WRITE_RESULT_DENIED;
        write->reply (write, &reply);
        return;
    }
    for (size_t i = 0; i < write->contents_len && !content; i++)
        if (is_plain_text (write->contents[i].mime))
            content = &write->contents[i];
    if (!content && write->contents_len > 0)
        content = &write->contents[0];

    g_autofree char *text = content ? g_strndup ((const char *) content->data.ptr, content->data.len)
                                    : g_strdup ("");
    self->host->write_text (self->host_data, clipboard_for (write->location), text);
    reply.result = GHOSTTY_CLIPBOARD_WRITE_RESULT_SUCCESS;
    write->reply (write, &reply);
}

typedef struct {
    gboolean done;
    gboolean expired;
    char *text;
} ClipboardWait;

static void
clipboard_wait_clear (gpointer data)
{
    g_free (((ClipboardWait *) data)->text);
}

static void
clipboard_wait_done (const char *text, gpointer data)
{
    ClipboardWait *wait = data;

    if (!wait->done) {
        wait->done = TRUE;
        wait->text = g_strdup (text);
    }
    g_rc_box_release_full (wait, clipboard_wait_clear);
}

static gboolean
clipboard_wait_expire (gpointer data)
{
    ((ClipboardWait *) data)->expired = TRUE;
    return G_SOURCE_REMOVE;
}

/* The read must be answered before this returns, so wait for the host's
 * async read in a nested main loop. Input, resize, close, and child exit
 * that arrive meanwhile are deferred until the write returns. */
static void
on_clipboard_read (GhosttyTerminal terminal, void *userdata, const GhosttyClipboardRead *read)
{
    CorralTermSession *self = userdata;
    GhosttyClipboardReadReply reply = GHOSTTY_INIT_SIZED (GhosttyClipboardReadReply);
    GhosttyString plain = { (const uint8_t *) "text/plain", 10 };
    GhosttyClipboardContent content = { plain, { NULL, 0 } };
    gboolean wants_text = read->mimes_len == 0;
    ClipboardWait *wait;
    guint timer;

    if (self->appearance.clipboard_read == CORRAL_CLIPBOARD_DENY) {
        reply.result = GHOSTTY_CLIPBOARD_READ_RESULT_DENIED;
        read->reply (read, &reply);
        return;
    }
    if (read->list && read->mimes_len == 0) {
        reply.result = GHOSTTY_CLIPBOARD_READ_RESULT_SUCCESS;
        reply.available = &plain;
        reply.available_len = 1;
        read->reply (read, &reply);
        return;
    }
    for (size_t i = 0; i < read->mimes_len; i++)
        wants_text = wants_text || is_plain_text (read->mimes[i]);
    if (!wants_text || !self->host || !self->host->read_text) {
        reply.result = GHOSTTY_CLIPBOARD_READ_RESULT_UNSUPPORTED;
        read->reply (read, &reply);
        return;
    }

    wait = g_rc_box_new0 (ClipboardWait);
    g_rc_box_acquire (wait);
    self->host->read_text (self->host_data, clipboard_for (read->location), clipboard_wait_done, wait);
    timer = g_timeout_add (self->clipboard_timeout_ms, clipboard_wait_expire, wait);
    while (!wait->done && !wait->expired)
        g_main_context_iteration (NULL, TRUE);
    if (!wait->expired)
        g_source_remove (timer);

    if (!wait->done) {
        g_warning ("clipboard read for OSC 52 timed out after %u ms", self->clipboard_timeout_ms);
        reply.result = GHOSTTY_CLIPBOARD_READ_RESULT_BUSY;
    } else if (!wait->text) {
        reply.result = GHOSTTY_CLIPBOARD_READ_RESULT_IO_ERROR;
    } else {
        content.data = (GhosttyString) { (const uint8_t *) wait->text, strlen (wait->text) };
        reply.result = GHOSTTY_CLIPBOARD_READ_RESULT_SUCCESS;
        reply.contents = &content;
        reply.contents_len = 1;
    }
    read->reply (read, &reply);
    g_rc_box_release_full (wait, clipboard_wait_clear);
}

static GhosttyCursorStyle
cursor_style (CorralCursorStyle style)
{
    switch (style) {
    case CORRAL_CURSOR_BAR:
        return GHOSTTY_CURSOR_STYLE_BAR;
    case CORRAL_CURSOR_UNDERLINE:
        return GHOSTTY_CURSOR_STYLE_UNDERLINE;
    case CORRAL_CURSOR_BLOCK_HOLLOW:
        return GHOSTTY_CURSOR_STYLE_BLOCK_HOLLOW;
    case CORRAL_CURSOR_BLOCK:
    default:
        return GHOSTTY_CURSOR_STYLE_BLOCK;
    }
}

static void
copy_appearance (CorralAppearance *dest, const CorralAppearance *src)
{
    *dest = *src;
    dest->term = g_strdup (src->term);
    dest->env = g_ptr_array_new_with_free_func (g_free);
    for (guint i = 0; src->env && i < src->env->len; i++)
        g_ptr_array_add (dest->env, g_strdup (g_ptr_array_index (src->env, i)));
}

static void
apply_appearance (CorralTermSession *self)
{
    GhosttyTerminal t = self->terminal;
    const CorralAppearance *a = &self->appearance;
    GhosttyCursorStyle style = cursor_style (a->cursor_style);
    bool blink = a->cursor_style_blink != 0;
    size_t scrollback_bytes = a->scrollback_limit_bytes;
    size_t scrollback_lines = a->scrollback_limit_lines == G_MAXSIZE ? SIZE_MAX : a->scrollback_limit_lines;
    uint64_t image_limit = a->image_storage_limit;
    bool on = true;
    const char *tmp = g_get_tmp_dir ();
    GhosttyString tmp_dir = { (const uint8_t *) tmp, strlen (tmp) };
    GhosttyString term = { (const uint8_t *) a->term, strlen (a->term) };

    ghostty_terminal_set (t, GHOSTTY_TERMINAL_OPT_COLOR_FOREGROUND, &a->foreground);
    ghostty_terminal_set (t, GHOSTTY_TERMINAL_OPT_COLOR_BACKGROUND, &a->background);
    ghostty_terminal_set (t, GHOSTTY_TERMINAL_OPT_COLOR_PALETTE, a->palette);
    if (a->cursor_color.kind == CORRAL_TERM_COLOR_RGB)
        ghostty_terminal_set (t, GHOSTTY_TERMINAL_OPT_COLOR_CURSOR, &a->cursor_color.rgb);
    ghostty_terminal_set (t, GHOSTTY_TERMINAL_OPT_DEFAULT_CURSOR_STYLE, &style);
    ghostty_terminal_set (t, GHOSTTY_TERMINAL_OPT_DEFAULT_CURSOR_BLINK, &blink);
    ghostty_terminal_set (t, GHOSTTY_TERMINAL_OPT_SCROLLBACK_MAX_BYTES, &scrollback_bytes);
    ghostty_terminal_set (t, GHOSTTY_TERMINAL_OPT_SCROLLBACK_MAX_LINES, &scrollback_lines);
    ghostty_terminal_set (t, GHOSTTY_TERMINAL_OPT_KITTY_IMAGE_STORAGE_LIMIT, &image_limit);
    ghostty_terminal_set (t, GHOSTTY_TERMINAL_OPT_KITTY_IMAGE_MEDIUM_FILE, &on);
    ghostty_terminal_set (t, GHOSTTY_TERMINAL_OPT_KITTY_IMAGE_MEDIUM_TEMP_FILE, &tmp_dir);
    ghostty_terminal_set (t, GHOSTTY_TERMINAL_OPT_KITTY_IMAGE_MEDIUM_SHARED_MEM, &on);
    ghostty_terminal_set (t, GHOSTTY_TERMINAL_OPT_TERMINFO_NAME, &term);
}

CorralTermSession *
corral_term_session_new (const CorralAppearance *appearance,
                         guint16 cols,
                         guint16 rows,
                         guint32 cell_width,
                         guint32 cell_height)
{
    CorralTermSession *self = g_object_new (CORRAL_TYPE_TERM_SESSION, NULL);
    GhosttyTerminal t = NULL;

    if (appearance)
        copy_appearance (&self->appearance, appearance);
    else
        corral_appearance_init (&self->appearance);

    self->cols = MAX (cols, 1);
    self->rows = MAX (rows, 1);
    self->cell_width = cell_width;
    self->cell_height = cell_height;

    if (ghostty_terminal_new (NULL, &t, self->cols, self->rows) != GHOSTTY_SUCCESS)
        g_error ("libghostty-vt could not create a terminal");
    self->terminal = t;
    ghostty_terminal_resize (t, self->cols, self->rows, cell_width, cell_height);

    ghostty_terminal_set (t, GHOSTTY_TERMINAL_OPT_USERDATA, self);
    ghostty_terminal_set (t, GHOSTTY_TERMINAL_OPT_WRITE_PTY, (const void *) on_write_pty);
    ghostty_terminal_set (t, GHOSTTY_TERMINAL_OPT_TITLE_CHANGED, (const void *) on_title_changed);
    ghostty_terminal_set (t, GHOSTTY_TERMINAL_OPT_BELL, (const void *) on_bell);
    ghostty_terminal_set (t, GHOSTTY_TERMINAL_OPT_SIZE, (const void *) on_size);
    ghostty_terminal_set (t, GHOSTTY_TERMINAL_OPT_XTVERSION, (const void *) on_xtversion);
    ghostty_terminal_set (t, GHOSTTY_TERMINAL_OPT_COLOR_SCHEME, (const void *) on_color_scheme);
    ghostty_terminal_set (t, GHOSTTY_TERMINAL_OPT_RENDER_HOLD, (const void *) on_render_hold);
    ghostty_terminal_set (t, GHOSTTY_TERMINAL_OPT_CLIPBOARD_WRITE, (const void *) on_clipboard_write);
    ghostty_terminal_set (t, GHOSTTY_TERMINAL_OPT_CLIPBOARD_READ, (const void *) on_clipboard_read);
    apply_appearance (self);

    if (ghostty_key_encoder_new (NULL, &self->key_encoder) != GHOSTTY_SUCCESS
        || ghostty_key_event_new (NULL, &self->key_event) != GHOSTTY_SUCCESS
        || ghostty_mouse_encoder_new (NULL, &self->mouse_encoder) != GHOSTTY_SUCCESS
        || ghostty_mouse_event_new (NULL, &self->mouse_event) != GHOSTTY_SUCCESS
        || ghostty_selection_gesture_new (NULL, &self->gesture) != GHOSTTY_SUCCESS)
        g_error ("libghostty-vt could not create the input encoders");

    return self;
}

static char *
exe_dir (void)
{
    g_autofree char *exe = g_file_read_link ("/proc/self/exe", NULL);

    return exe ? g_path_get_dirname (exe) : NULL;
}

char **
corral_term_session_child_env (const CorralAppearance *appearance, char **parent_env)
{
    g_auto (GStrv) env = g_strdupv (parent_env);
    g_autofree char *bin_dir = exe_dir ();
    g_autofree char *features = corral_appearance_shell_features (appearance);
    const char *resources = g_environ_getenv (env, "GHOSTTY_RESOURCES_DIR");
    g_autofree char *surface_id = NULL;
    guint64 id;

    for (char **e = parent_env; e && *e; e++) {
        if (!g_str_has_prefix (*e, "HERDR_"))
            continue;
        g_autofree char *key = g_strndup (*e, strcspn (*e, "="));
        if (!g_str_equal (key, "HERDR_SOCKET_PATH") && !g_str_equal (key, "HERDR_CONFIG_PATH"))
            env = g_environ_unsetenv (env, key);
    }
    env = g_environ_unsetenv (env, "VTE_VERSION");
    env = g_environ_unsetenv (env, "GHOSTTY_LOG");

    if (resources && *resources) {
        g_autofree char *parent = g_path_get_dirname (resources);
        g_autofree char *terminfo = g_build_filename (parent, "terminfo", NULL);
        env = g_environ_setenv (env, "TERM", appearance->term, TRUE);
        env = g_environ_setenv (env, "TERMINFO", terminfo, TRUE);
    } else {
        env = g_environ_setenv (env, "TERM", "xterm-256color", TRUE);
    }
    env = g_environ_setenv (env, "COLORTERM", "truecolor", TRUE);

    if (bin_dir) {
        const char *path = g_environ_getenv (env, "PATH");
        env = g_environ_setenv (env, "GHOSTTY_BIN_DIR", bin_dir, TRUE);
        if (!path || !*path) {
            env = g_environ_setenv (env, "PATH", bin_dir, TRUE);
        } else {
            g_auto (GStrv) parts = g_strsplit (path, ":", -1);
            if (!g_strv_contains ((const char *const *) parts, bin_dir)) {
                g_autofree char *joined = g_strconcat (path, ":", bin_dir, NULL);
                env = g_environ_setenv (env, "PATH", joined, TRUE);
            }
        }
    }

    env = g_environ_setenv (env, "TERM_PROGRAM", "ghostty", TRUE);
    env = g_environ_setenv (env, "TERM_PROGRAM_VERSION", EMBED_VERSION, TRUE);

    do
        id = ((guint64) g_random_int () << 32) | g_random_int ();
    while (id == 0);
    surface_id = g_strdup_printf ("0x%016" G_GINT64_MODIFIER "x", id);
    env = g_environ_setenv (env, "GHOSTTY_SURFACE_ID", surface_id, TRUE);

    if (features)
        env = g_environ_setenv (env, "GHOSTTY_SHELL_FEATURES", features, TRUE);
    else
        env = g_environ_unsetenv (env, "GHOSTTY_SHELL_FEATURES");

    for (guint i = 0; appearance->env && i < appearance->env->len; i++) {
        const char *pair = g_ptr_array_index (appearance->env, i);
        const char *eq = strchr (pair, '=');
        if (!eq)
            continue;
        g_autofree char *key = g_strndup (pair, eq - pair);
        if (eq[1] == '\0')
            env = g_environ_unsetenv (env, key);
        else
            env = g_environ_setenv (env, key, eq + 1, TRUE);
    }

    return g_steal_pointer (&env);
}

static void
set_winsize (CorralTermSession *self)
{
    struct winsize ws = {
        .ws_row = self->rows,
        .ws_col = self->cols,
        .ws_xpixel = (unsigned short) MIN ((guint32) self->cols * self->cell_width, G_MAXUSHORT),
        .ws_ypixel = (unsigned short) MIN ((guint32) self->rows * self->cell_height, G_MAXUSHORT),
    };

    if (self->master >= 0)
        ioctl (self->master, TIOCSWINSZ, &ws);
}

typedef enum {
    READ_DATA,
    READ_AGAIN,
    READ_END,
} ReadResult;

static ReadResult
read_chunk (CorralTermSession *self)
{
    guint8 buf[READ_CHUNK];
    ssize_t n;

    do
        n = read (self->master, buf, sizeof buf);
    while (n < 0 && errno == EINTR);

    if (n > 0) {
        feed (self, buf, (gsize) n);
        return READ_DATA;
    }
    return n < 0 && errno == EAGAIN ? READ_AGAIN : READ_END;
}

static gboolean
on_readable (int fd, GIOCondition condition, gpointer user_data)
{
    g_autoptr (CorralTermSession) self = g_object_ref (user_data);
    ReadResult result = read_chunk (self);

    if (result == READ_DATA)
        g_signal_emit (self, signals[SIGNAL_CHANGED], 0);
    if (result != READ_END || self->read_source == 0)
        return G_SOURCE_CONTINUE;
    self->read_source = 0;
    return G_SOURCE_REMOVE;
}

static void
finish_exit (CorralTermSession *self, int status)
{
    g_autoptr (CorralTermSession) hold = g_object_ref (self);

    /* Output the child wrote just before exiting is still in the pty. */
    for (int i = 0; i < 64 && self->read_source; i++)
        if (read_chunk (self) != READ_DATA)
            break;
    g_clear_handle_id (&self->read_source, g_source_remove);

    feed (self, (const guint8 *) EXITED_LINE, strlen (EXITED_LINE));
    g_signal_emit (self, signals[SIGNAL_CHANGED], 0);
    g_signal_emit (self, signals[SIGNAL_EXITED], 0, status);
}

static void
on_child_exited (GPid pid, int wait_status, gpointer user_data)
{
    CorralTermSession *self = user_data;
    int status = WIFEXITED (wait_status) ? WEXITSTATUS (wait_status)
               : WIFSIGNALED (wait_status) ? 128 + WTERMSIG (wait_status)
               : 1;

    self->child_watch = 0;
    self->child_done = TRUE;
    g_spawn_close_pid (pid);

    if (self->in_vt_write) {
        self->exit_deferred = TRUE;
        self->exit_status = status;
        return;
    }
    finish_exit (self, status);
}

static void
child_setup (gpointer user_data)
{
    setsid ();
    ioctl (STDIN_FILENO, TIOCSCTTY, 0);
}

gboolean
corral_term_session_spawn (CorralTermSession *self,
                           const char *const *argv,
                           const char *cwd,
                           GError **error)
{
    g_auto (GStrv) parent_env = g_get_environ ();
    g_auto (GStrv) env = NULL;
    char slave_name[128];
    struct termios tio;
    int master;
    int slave;

    g_return_val_if_fail (CORRAL_IS_TERM_SESSION (self), FALSE);
    g_return_val_if_fail (argv && argv[0], FALSE);
    g_return_val_if_fail (self->master < 0 && self->pid == 0, FALSE);

    master = posix_openpt (O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (master < 0 || grantpt (master) != 0 || unlockpt (master) != 0
        || ptsname_r (master, slave_name, sizeof slave_name) != 0) {
        int saved = errno;
        if (master >= 0)
            close (master);
        g_set_error (error, G_IO_ERROR, g_io_error_from_errno (saved),
                     "Could not open a pty: %s", g_strerror (saved));
        return FALSE;
    }
    slave = open (slave_name, O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (slave < 0) {
        int saved = errno;
        close (master);
        g_set_error (error, G_IO_ERROR, g_io_error_from_errno (saved),
                     "Could not open %s: %s", slave_name, g_strerror (saved));
        return FALSE;
    }
    if (tcgetattr (slave, &tio) == 0) {
        tio.c_iflag |= IUTF8;
        tcsetattr (slave, TCSANOW, &tio);
    }

    self->master = master;
    set_winsize (self);

    env = corral_term_session_child_env (&self->appearance, parent_env);
    if (!g_spawn_async_with_pipes_and_fds (cwd, argv, (const char *const *) env,
                                           G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_SEARCH_PATH_FROM_ENVP,
                                           child_setup, NULL,
                                           slave, slave, slave,
                                           NULL, NULL, 0,
                                           &self->pid, NULL, NULL, NULL,
                                           error)) {
        close (slave);
        close (master);
        self->master = -1;
        self->pid = 0;
        return FALSE;
    }
    close (slave);

    g_unix_set_fd_nonblocking (master, TRUE, NULL);
    self->read_source = g_unix_fd_add (master, G_IO_IN | G_IO_HUP | G_IO_ERR, on_readable, self);
    self->child_watch = g_child_watch_add (self->pid, on_child_exited, self);
    return TRUE;
}

void
corral_term_session_resize (CorralTermSession *self,
                            guint16 cols,
                            guint16 rows,
                            guint32 cell_width,
                            guint32 cell_height)
{
    g_return_if_fail (CORRAL_IS_TERM_SESSION (self));

    cols = MAX (cols, 1);
    rows = MAX (rows, 1);
    if (cols == self->cols && rows == self->rows
        && cell_width == self->cell_width && cell_height == self->cell_height)
        return;

    self->cols = cols;
    self->rows = rows;
    self->cell_width = cell_width;
    self->cell_height = cell_height;
    if (self->in_vt_write) {
        self->resize_deferred = TRUE;
        return;
    }
    apply_resize (self);
}

static void
apply_resize (CorralTermSession *self)
{
    ghostty_terminal_resize (self->terminal, self->cols, self->rows, self->cell_width, self->cell_height);
    set_winsize (self);
    g_signal_emit (self, signals[SIGNAL_CHANGED], 0);
}

void
corral_term_session_set_padding (CorralTermSession *self,
                                 guint left,
                                 guint top,
                                 guint right,
                                 guint bottom)
{
    g_return_if_fail (CORRAL_IS_TERM_SESSION (self));
    self->padding_left = left;
    self->padding_top = top;
    self->padding_right = right;
    self->padding_bottom = bottom;
}

void
corral_term_session_write (CorralTermSession *self, const void *data, gsize len)
{
    g_return_if_fail (CORRAL_IS_TERM_SESSION (self));
    queue_output (self, data, len);
}

void
corral_term_session_set_host (CorralTermSession *self, const CorralTermHost *host, gpointer host_data)
{
    g_return_if_fail (CORRAL_IS_TERM_SESSION (self));
    self->host = host;
    self->host_data = host_data;
}

void
corral_term_session_set_clipboard_timeout (CorralTermSession *self, guint ms)
{
    g_return_if_fail (CORRAL_IS_TERM_SESSION (self));
    self->clipboard_timeout_ms = ms;
}

gboolean
corral_term_session_is_busy (CorralTermSession *self)
{
    g_return_val_if_fail (CORRAL_IS_TERM_SESSION (self), FALSE);
    return self->in_vt_write;
}

static gboolean
mode_on (CorralTermSession *self, GhosttyMode mode)
{
    GhosttyTerminalModeConfig config = { mode, false };

    ghostty_terminal_get (self->terminal, GHOSTTY_TERMINAL_DATA_MODE, &config);
    return config.value;
}

static gboolean
mouse_tracking (CorralTermSession *self)
{
    bool tracking = false;

    ghostty_terminal_get (self->terminal, GHOSTTY_TERMINAL_DATA_MOUSE_TRACKING, &tracking);
    return tracking;
}

static void
scroll_to_bottom (CorralTermSession *self)
{
    GhosttyTerminalScrollViewport bottom = { .tag = GHOSTTY_TERMINAL_SCROLL_VIEWPORT_BOTTOM };

    ghostty_terminal_scroll_viewport (self->terminal, bottom);
}

static gboolean
get_selection (CorralTermSession *self, GhosttySelection *selection)
{
    return ghostty_terminal_get (self->terminal, GHOSTTY_TERMINAL_DATA_SELECTION, selection) == GHOSTTY_SUCCESS;
}

static void
set_selection (CorralTermSession *self, const GhosttySelection *selection)
{
    ghostty_terminal_set (self->terminal, GHOSTTY_TERMINAL_OPT_SELECTION, selection);
}

char *
corral_term_session_selection_text (CorralTermSession *self)
{
    GhosttySelection selection;
    GhosttyFormatter formatter = NULL;
    bool unwrap = true;
    bool trim = self->appearance.clipboard_trim_trailing_spaces;
    uint8_t *buf = NULL;
    size_t len = 0;
    char *text = NULL;

    g_return_val_if_fail (CORRAL_IS_TERM_SESSION (self), NULL);
    if (!get_selection (self, &selection))
        return NULL;
    if (ghostty_formatter_terminal_new (NULL, &formatter, self->terminal) != GHOSTTY_SUCCESS)
        return NULL;
    ghostty_formatter_set (formatter, GHOSTTY_FORMATTER_OPT_SELECTION, &selection);
    ghostty_formatter_set (formatter, GHOSTTY_FORMATTER_OPT_UNWRAP, &unwrap);
    ghostty_formatter_set (formatter, GHOSTTY_FORMATTER_OPT_TRIM, &trim);
    if (ghostty_formatter_format_alloc (formatter, NULL, &buf, &len) == GHOSTTY_SUCCESS) {
        text = len > 0 ? g_strndup ((const char *) buf, len) : NULL;
        ghostty_free (NULL, buf, len);
    }
    ghostty_formatter_free (formatter);
    return text;
}

static void
write_clipboard (CorralTermSession *self, CorralTermClipboard which, const char *text)
{
    if (self->host && self->host->write_text)
        self->host->write_text (self->host_data, which, text);
}

static void
copy_selection (CorralTermSession *self)
{
    g_autofree char *text = corral_term_session_selection_text (self);

    if (!text)
        return;
    write_clipboard (self, CORRAL_TERM_CLIPBOARD_STANDARD, text);
    if (self->appearance.selection_clear_on_copy)
        set_selection (self, NULL);
}

static void
copy_on_select (CorralTermSession *self)
{
    CorralCopyOnSelect mode = self->appearance.copy_on_select;
    g_autofree char *text = NULL;

    if (mode == CORRAL_COPY_ON_SELECT_NONE)
        return;
    text = corral_term_session_selection_text (self);
    if (!text)
        return;
    if (mode == CORRAL_COPY_ON_SELECT_PRIMARY || mode == CORRAL_COPY_ON_SELECT_BOTH)
        write_clipboard (self, CORRAL_TERM_CLIPBOARD_PRIMARY, text);
    if (mode == CORRAL_COPY_ON_SELECT_CLIPBOARD || mode == CORRAL_COPY_ON_SELECT_BOTH)
        write_clipboard (self, CORRAL_TERM_CLIPBOARD_STANDARD, text);
}

typedef struct {
    GWeakRef session;
    CorralTermClipboard which;
} PasteRequest;

static void
on_paste_text (const char *text, gpointer data)
{
    PasteRequest *request = data;
    g_autoptr (CorralTermSession) self = g_weak_ref_get (&request->session);

    if (self && text)
        corral_term_session_paste (self, request->which, text);
    g_weak_ref_clear (&request->session);
    g_free (request);
}

static void
request_paste (CorralTermSession *self, CorralTermClipboard which)
{
    PasteRequest *request;

    if (!self->host || !self->host->read_text)
        return;
    request = g_new0 (PasteRequest, 1);
    g_weak_ref_init (&request->session, self);
    request->which = which;
    self->host->read_text (self->host_data, which, on_paste_text, request);
}

typedef struct {
    const char *data;
    gsize len;
} PasteData;

static bool
read_paste (void *userdata, GhosttyString mime, GhosttyWriter writer)
{
    PasteData *paste = userdata;

    return paste->len == 0 || writer.write (writer.userdata, (const uint8_t *) paste->data, paste->len);
}

static void
handle_paste (CorralTermSession *self, CorralTermClipboard from, const char *text)
{
    GhosttyString mime = { (const uint8_t *) "text/plain", 10 };
    PasteData data = { text, strlen (text) };
    GhosttyPaste paste = GHOSTTY_INIT_SIZED (GhosttyPaste);
    GhosttyResult result;

    paste.location = from == CORRAL_TERM_CLIPBOARD_PRIMARY ? GHOSTTY_CLIPBOARD_LOCATION_PRIMARY
                                                           : GHOSTTY_CLIPBOARD_LOCATION_STANDARD;
    paste.source = GHOSTTY_PASTE_SOURCE_CLIPBOARD;
    paste.mimes = &mime;
    paste.mimes_len = 1;
    paste.reader = (GhosttyMimeReader) { read_paste, &data };
    /* The embed confirmed unsafe pastes without asking. */
    paste.allow_unsafe = true;
    result = ghostty_terminal_paste (self->terminal, &paste, NULL);
    if (result != GHOSTTY_SUCCESS)
        g_warning ("paste failed: libghostty-vt result %d", result);
    scroll_to_bottom (self);
    g_signal_emit (self, signals[SIGNAL_CHANGED], 0);
}

static GhosttyKey
physical_key (guint keycode)
{
    gsize lo = 0;
    gsize hi = G_N_ELEMENTS (corral_xkb_keys);

    while (lo < hi) {
        gsize mid = (lo + hi) / 2;
        if (corral_xkb_keys[mid].xkb == keycode)
            return corral_xkb_keys[mid].key;
        if (corral_xkb_keys[mid].xkb < keycode)
            lo = mid + 1;
        else
            hi = mid;
    }
    return GHOSTTY_KEY_UNIDENTIFIED;
}

typedef enum {
    CHORD_NONE,
    CHORD_COPY,
    CHORD_PASTE,
    CHORD_PASTE_PRIMARY,
} Chord;

/* The binds Corral kept in the embed's keybind overlay. */
static Chord
host_chord (const CorralTermKey *key, GhosttyKey physical)
{
    GhosttyMods mods = key->mods & MOD_KEYS;
    gunichar unshifted = g_unichar_tolower (key->unshifted_codepoint);

    if (physical == GHOSTTY_KEY_COPY && mods == 0)
        return CHORD_COPY;
    if (physical == GHOSTTY_KEY_PASTE && mods == 0)
        return CHORD_PASTE;
    if (physical == GHOSTTY_KEY_INSERT && mods == GHOSTTY_MODS_CTRL)
        return CHORD_COPY;
    if (physical == GHOSTTY_KEY_INSERT && mods == GHOSTTY_MODS_SHIFT)
        return CHORD_PASTE_PRIMARY;
    if (mods == (GHOSTTY_MODS_CTRL | GHOSTTY_MODS_SHIFT) && unshifted == 'c')
        return CHORD_COPY;
    if (mods == (GHOSTTY_MODS_CTRL | GHOSTTY_MODS_SHIFT) && unshifted == 'v')
        return CHORD_PASTE;
    return CHORD_NONE;
}

static gboolean
printable (const char *text)
{
    if (!text || !*text)
        return FALSE;
    for (const guchar *p = (const guchar *) text; *p; p++)
        if (*p < 0x20 || *p == 0x7f)
            return FALSE;
    return TRUE;
}

static gboolean
handle_key (CorralTermSession *self, const CorralTermKey *key)
{
    GhosttyKey physical = physical_key (key->keycode);
    Chord chord = host_chord (key, physical);
    const char *text = printable (key->text) ? key->text : NULL;
    uint8_t buf[128];
    size_t written = 0;

    if (chord != CHORD_NONE) {
        if (key->action == GHOSTTY_KEY_ACTION_RELEASE)
            return TRUE;
        if (chord == CHORD_COPY)
            copy_selection (self);
        else
            request_paste (self, chord == CHORD_PASTE ? CORRAL_TERM_CLIPBOARD_STANDARD
                                                      : CORRAL_TERM_CLIPBOARD_PRIMARY);
        g_signal_emit (self, signals[SIGNAL_CHANGED], 0);
        return TRUE;
    }

    ghostty_key_encoder_set_from_terminal (self->key_encoder, self->terminal);
    ghostty_key_event_set_action (self->key_event, key->action);
    ghostty_key_event_set_key (self->key_event, physical);
    ghostty_key_event_set_mods (self->key_event, key->mods);
    ghostty_key_event_set_consumed_mods (self->key_event, key->consumed_mods);
    ghostty_key_event_set_unshifted_codepoint (self->key_event, key->unshifted_codepoint);
    ghostty_key_event_set_utf8 (self->key_event,
                                (GhosttyString) { (const uint8_t *) text, text ? strlen (text) : 0 });

    if (ghostty_key_encoder_encode (self->key_encoder, self->key_event, buf, sizeof buf, &written)
        == GHOSTTY_OUT_OF_SPACE) {
        g_autofree uint8_t *big = g_malloc (written);
        if (ghostty_key_encoder_encode (self->key_encoder, self->key_event, big, written, &written)
            == GHOSTTY_SUCCESS)
            queue_output (self, big, written);
    } else if (written > 0) {
        queue_output (self, buf, written);
    }
    ghostty_key_event_set_utf8 (self->key_event, (GhosttyString) { NULL, 0 });
    if (written == 0)
        return FALSE;

    if (self->appearance.selection_clear_on_typing)
        set_selection (self, NULL);
    scroll_to_bottom (self);
    g_signal_emit (self, signals[SIGNAL_CHANGED], 0);
    return TRUE;
}

static GhosttyPointCoordinate
cell_at (CorralTermSession *self, double x, double y)
{
    double col = self->cell_width ? (x - self->padding_left) / self->cell_width : 0;
    double row = self->cell_height ? (y - self->padding_top) / self->cell_height : 0;

    return (GhosttyPointCoordinate) {
        .x = (uint16_t) CLAMP (col, 0, self->cols - 1),
        .y = (uint32_t) CLAMP (row, 0, self->rows - 1),
    };
}

static gboolean
ref_at (CorralTermSession *self, GhosttyPointCoordinate cell, GhosttyGridRef *ref)
{
    GhosttyPoint point = { .tag = GHOSTTY_POINT_TAG_VIEWPORT, .value.coordinate = cell };

    return ghostty_terminal_grid_ref (self->terminal, point, ref) == GHOSTTY_SUCCESS;
}

static GhosttyMouseButton
mouse_button (guint button)
{
    switch (button) {
    case 1:
        return GHOSTTY_MOUSE_BUTTON_LEFT;
    case 2:
        return GHOSTTY_MOUSE_BUTTON_MIDDLE;
    case 3:
        return GHOSTTY_MOUSE_BUTTON_RIGHT;
    default:
        return button >= 4 && button <= 11 ? GHOSTTY_MOUSE_BUTTON_FOUR + (button - 4)
                                           : GHOSTTY_MOUSE_BUTTON_UNKNOWN;
    }
}

/* Button-event tracking drops a motion that names no button. Ghostty
 * reports the first button still held, the same way a drag is encoded. */
static GhosttyMouseButton
held_button (guint buttons_down)
{
    for (guint button = 1; button <= 11; button++) {
        if (buttons_down & (1u << button))
            return mouse_button (button);
    }
    return GHOSTTY_MOUSE_BUTTON_UNKNOWN;
}

static void
report_mouse (CorralTermSession *self, GhosttyMouseAction action, GhosttyMouseButton button,
              double x, double y, GhosttyMods mods)
{
    GhosttyMouseEncoderSize size = {
        .size = sizeof size,
        .screen_width = self->padding_left + self->cols * self->cell_width + self->padding_right,
        .screen_height = self->padding_top + self->rows * self->cell_height + self->padding_bottom,
        .cell_width = MAX (self->cell_width, 1),
        .cell_height = MAX (self->cell_height, 1),
        .padding_top = self->padding_top,
        .padding_bottom = self->padding_bottom,
        .padding_right = self->padding_right,
        .padding_left = self->padding_left,
    };
    bool any_pressed = self->buttons_down != 0;
    bool track_last_cell = true;
    uint8_t buf[64];
    size_t written = 0;

    ghostty_mouse_encoder_set_from_terminal (self->mouse_encoder, self->terminal);
    ghostty_mouse_encoder_set (self->mouse_encoder, GHOSTTY_MOUSE_ENCODER_OPT_SIZE, &size);
    ghostty_mouse_encoder_set (self->mouse_encoder, GHOSTTY_MOUSE_ENCODER_OPT_ANY_BUTTON_PRESSED, &any_pressed);
    ghostty_mouse_encoder_set (self->mouse_encoder, GHOSTTY_MOUSE_ENCODER_OPT_TRACK_LAST_CELL, &track_last_cell);
    ghostty_mouse_event_set_action (self->mouse_event, action);
    if (button == GHOSTTY_MOUSE_BUTTON_UNKNOWN)
        ghostty_mouse_event_clear_button (self->mouse_event);
    else
        ghostty_mouse_event_set_button (self->mouse_event, button);
    ghostty_mouse_event_set_mods (self->mouse_event, mods);
    ghostty_mouse_event_set_position (self->mouse_event, (GhosttyMousePosition) { (float) x, (float) y });
    if (ghostty_mouse_encoder_encode (self->mouse_encoder, self->mouse_event, buf, sizeof buf, &written)
            == GHOSTTY_SUCCESS
        && written > 0)
        queue_output (self, buf, written);
}

static GhosttySelectionGestureGeometry
gesture_geometry (CorralTermSession *self)
{
    return (GhosttySelectionGestureGeometry) {
        .columns = self->cols,
        .cell_width = MAX (self->cell_width, 1),
        .padding_left = self->padding_left,
        .screen_height = MAX (self->padding_top + self->rows * self->cell_height + self->padding_bottom, 1),
    };
}

static gboolean
rectangle_mods (GhosttyMods mods)
{
    return (mods & GHOSTTY_MODS_CTRL) && (mods & GHOSTTY_MODS_ALT);
}

static GhosttySelectionGestureAutoscroll
gesture_autoscroll (CorralTermSession *self)
{
    GhosttySelectionGestureAutoscroll autoscroll = GHOSTTY_SELECTION_GESTURE_AUTOSCROLL_NONE;

    ghostty_selection_gesture_get (self->gesture, self->terminal, GHOSTTY_SELECTION_GESTURE_DATA_AUTOSCROLL,
                                   &autoscroll);
    return autoscroll;
}

static gboolean
on_autoscroll (gpointer user_data)
{
    CorralTermSession *self = user_data;
    GhosttySelectionGestureEvent event = NULL;
    GhosttySelectionGestureGeometry geometry = gesture_geometry (self);
    GhosttySurfacePosition position = { self->mouse_x, self->mouse_y };
    GhosttyPointCoordinate viewport = cell_at (self, self->mouse_x, self->mouse_y);
    bool rectangle = rectangle_mods (self->mouse_mods);
    GhosttySelection selection;

    if (self->in_vt_write)
        return G_SOURCE_CONTINUE;
    if (!(self->buttons_down & (1u << 1)) || gesture_autoscroll (self) == GHOSTTY_SELECTION_GESTURE_AUTOSCROLL_NONE) {
        self->autoscroll_source = 0;
        return G_SOURCE_REMOVE;
    }
    ghostty_selection_gesture_event_new (NULL, &event, GHOSTTY_SELECTION_GESTURE_EVENT_TYPE_AUTOSCROLL_TICK);
    ghostty_selection_gesture_event_set (event, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_VIEWPORT, &viewport);
    ghostty_selection_gesture_event_set (event, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_GEOMETRY, &geometry);
    ghostty_selection_gesture_event_set (event, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_POSITION, &position);
    ghostty_selection_gesture_event_set (event, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_RECTANGLE, &rectangle);
    if (ghostty_selection_gesture_event (self->gesture, self->terminal, event, &selection) == GHOSTTY_SUCCESS)
        set_selection (self, &selection);
    ghostty_selection_gesture_event_free (event);
    g_signal_emit (self, signals[SIGNAL_CHANGED], 0);
    return G_SOURCE_CONTINUE;
}

#define SCHEME_CHARS "[\\w\\-.~:/?#@!$&*+,;=%]"
#define PATH_CHARS "[\\w\\-.~:\\/?#@!$&*+;=%]"
#define DOTTED_LOOKAHEAD "(?=" PATH_CHARS "*\\.)"
#define NO_TRAILING_COLON "(?<!:)"
#define SPACE_SEGMENT_START "(?<!:) (?!\\w+:\\/\\/)(?!\\.{0,2}\\/)(?!~\\/)"

/* Ghostty 1.4's link-url regex (src/config/url.zig). PCRE has no
 * unbounded lookbehind, so the bare-path branch's (?<!\$\d*) is cut to
 * one digit. */
static const char url_regex[] =
    "(?:https?://|mailto:|ftp://|file:|ssh:|git://|ssh://|tel:|magnet:|ipfs://|ipns://|gemini://|gopher://|news:)"
    "(?:(?:\\[[:0-9a-fA-F]+(?:[:0-9a-fA-F]*)+\\](?::[0-9]+)?)|" SCHEME_CHARS "+(?:[\\(\\[]\\w*[\\)\\]])?)+"
    "(?<![,.])"
    "|"
    "(?:\\.\\.\\/|\\.\\/|(?<!\\w)~\\/|(?:[\\w][\\w\\-.]*\\/)*(?<!\\w)\\$[A-Za-z_]\\w*\\/|\\.[\\w][\\w\\-.]*\\/"
    "|(?<![\\w~\\/])\\/(?!\\/))"
    "(?:" DOTTED_LOOKAHEAD PATH_CHARS "+(?:" SPACE_SEGMENT_START PATH_CHARS "*[\\/.])*" NO_TRAILING_COLON
    "|(?!" PATH_CHARS "*\\.)" PATH_CHARS "+(?:" SPACE_SEGMENT_START PATH_CHARS "+)*" NO_TRAILING_COLON ")"
    "|"
    DOTTED_LOOKAHEAD "(?<!\\$)(?<!\\$\\d)(?<!\\w)[\\w][\\w\\-.]*\\/" PATH_CHARS "+" NO_TRAILING_COLON;

static GRegex *
link_regex (void)
{
    static GRegex *regex;
    static gsize once;

    if (g_once_init_enter (&once)) {
        g_autoptr (GError) error = NULL;
        regex = g_regex_new (url_regex, G_REGEX_OPTIMIZE, 0, &error);
        if (!regex)
            g_warning ("link regex: %s", error->message);
        g_once_init_leave (&once, 1);
    }
    return regex;
}

static char *
link_at (CorralTermSession *self, GhosttyPointCoordinate cell)
{
    GhosttyGridRef ref;
    size_t len = 0;
    g_autoptr (GString) row = g_string_new (NULL);
    g_autofree gsize *offsets = g_new0 (gsize, self->cols + 1);
    g_autoptr (GMatchInfo) match = NULL;
    GRegex *regex;

    if (!ref_at (self, cell, &ref))
        return NULL;
    if (ghostty_grid_ref_hyperlink_uri (&ref, NULL, 0, &len) == GHOSTTY_OUT_OF_SPACE && len > 0) {
        char *uri = g_malloc0 (len + 1);
        if (ghostty_grid_ref_hyperlink_uri (&ref, (uint8_t *) uri, len, &len) == GHOSTTY_SUCCESS)
            return uri;
        g_free (uri);
    }
    if (!self->appearance.link_url || !(regex = link_regex ()))
        return NULL;

    for (guint16 col = 0; col < self->cols; col++) {
        GhosttyPointCoordinate at = { col, cell.y };
        uint32_t cps[16];
        size_t n = 0;

        offsets[col] = row->len;
        if (ref_at (self, at, &ref))
            ghostty_grid_ref_graphemes (&ref, cps, G_N_ELEMENTS (cps), &n);
        if (n == 0)
            g_string_append_c (row, ' ');
        for (size_t i = 0; i < n && i < G_N_ELEMENTS (cps); i++)
            g_string_append_unichar (row, cps[i]);
    }
    offsets[self->cols] = row->len;

    g_regex_match (regex, row->str, 0, &match);
    while (g_match_info_matches (match)) {
        int start = 0;
        int end = 0;
        g_match_info_fetch_pos (match, 0, &start, &end);
        if ((gsize) start <= offsets[cell.x] && offsets[cell.x] < (gsize) end)
            return g_match_info_fetch (match, 0);
        g_match_info_next (match, NULL);
    }
    return NULL;
}

static void
drag_to (CorralTermSession *self, const GhosttyGridRef *ref, double x, double y, GhosttyMods mods)
{
    GhosttySelectionGestureEvent event = NULL;
    GhosttySelectionGestureGeometry geometry = gesture_geometry (self);
    GhosttySurfacePosition position = { x, y };
    bool rectangle = rectangle_mods (mods);
    GhosttySelection selection;

    ghostty_selection_gesture_event_new (NULL, &event, GHOSTTY_SELECTION_GESTURE_EVENT_TYPE_DRAG);
    ghostty_selection_gesture_event_set (event, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_REF, ref);
    ghostty_selection_gesture_event_set (event, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_GEOMETRY, &geometry);
    ghostty_selection_gesture_event_set (event, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_POSITION, &position);
    ghostty_selection_gesture_event_set (event, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_RECTANGLE, &rectangle);
    if (ghostty_selection_gesture_event (self->gesture, self->terminal, event, &selection) == GHOSTTY_SUCCESS)
        set_selection (self, &selection);
    ghostty_selection_gesture_event_free (event);

    if (gesture_autoscroll (self) != GHOSTTY_SELECTION_GESTURE_AUTOSCROLL_NONE && !self->autoscroll_source)
        self->autoscroll_source = g_timeout_add (AUTOSCROLL_MS, on_autoscroll, self);
    g_signal_emit (self, signals[SIGNAL_CHANGED], 0);
}

/* Shift-click continues the last left-click gesture as a drag, unless it
 * comes fast enough to count as a repeat click. */
static gboolean
extend_selection (CorralTermSession *self, const Input *input, const GhosttyGridRef *ref)
{
    GhosttySelection selection;
    uint8_t clicks = 0;

    ghostty_selection_gesture_get (self->gesture, self->terminal, GHOSTTY_SELECTION_GESTURE_DATA_CLICK_COUNT,
                                   &clicks);
    if (clicks == 0 || !get_selection (self, &selection))
        return FALSE;
    if ((guint64) (input->time_us - self->last_click_us) * 1000 <= REPEAT_CLICK_NS)
        return FALSE;
    drag_to (self, ref, input->x, input->y, input->mods);
    return TRUE;
}

static void
left_press (CorralTermSession *self, const Input *input, const GhosttyGridRef *ref)
{
    GhosttySelectionGestureEvent event = NULL;
    GhosttySurfacePosition position = { input->x, input->y };
    uint64_t time_ns = (uint64_t) input->time_us * 1000;
    uint64_t interval_ns = REPEAT_CLICK_NS;
    double distance = MAX (self->cell_width, 1);
    GhosttySelectionGestureBehaviors behaviors = {
        GHOSTTY_SELECTION_GESTURE_BEHAVIOR_CELL,
        GHOSTTY_SELECTION_GESTURE_BEHAVIOR_WORD,
        (input->mods & (GHOSTTY_MODS_CTRL | GHOSTTY_MODS_SUPER)) ? GHOSTTY_SELECTION_GESTURE_BEHAVIOR_OUTPUT
                                                                 : GHOSTTY_SELECTION_GESTURE_BEHAVIOR_LINE,
    };
    GhosttySelection selection;
    uint8_t clicks = 0;

    self->last_click_us = input->time_us;
    ghostty_selection_gesture_event_new (NULL, &event, GHOSTTY_SELECTION_GESTURE_EVENT_TYPE_PRESS);
    ghostty_selection_gesture_event_set (event, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_REF, ref);
    ghostty_selection_gesture_event_set (event, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_POSITION, &position);
    ghostty_selection_gesture_event_set (event, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_TIME_NS, &time_ns);
    ghostty_selection_gesture_event_set (event, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_REPEAT_INTERVAL_NS,
                                         &interval_ns);
    ghostty_selection_gesture_event_set (event, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_REPEAT_DISTANCE, &distance);
    ghostty_selection_gesture_event_set (event, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_BEHAVIORS, &behaviors);
    if (ghostty_selection_gesture_event (self->gesture, self->terminal, event, &selection) == GHOSTTY_SUCCESS) {
        set_selection (self, &selection);
    } else {
        ghostty_selection_gesture_get (self->gesture, self->terminal, GHOSTTY_SELECTION_GESTURE_DATA_CLICK_COUNT,
                                       &clicks);
        if (clicks == 1)
            set_selection (self, NULL);
    }
    ghostty_selection_gesture_event_free (event);
    g_signal_emit (self, signals[SIGNAL_CHANGED], 0);
}

static void
left_release (CorralTermSession *self, const GhosttyGridRef *ref, GhosttyPointCoordinate cell, GhosttyMods mods)
{
    GhosttySelectionGestureEvent event = NULL;
    GhosttySelection selection;
    bool dragged = false;

    ghostty_selection_gesture_event_new (NULL, &event, GHOSTTY_SELECTION_GESTURE_EVENT_TYPE_RELEASE);
    ghostty_selection_gesture_event_set (event, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_REF, ref);
    ghostty_selection_gesture_event (self->gesture, self->terminal, event, NULL);
    ghostty_selection_gesture_event_free (event);
    g_clear_handle_id (&self->autoscroll_source, g_source_remove);
    ghostty_selection_gesture_get (self->gesture, self->terminal, GHOSTTY_SELECTION_GESTURE_DATA_DRAGGED, &dragged);

    if (get_selection (self, &selection))
        copy_on_select (self);
    if (!dragged && (mods & GHOSTTY_MODS_CTRL)) {
        g_autofree char *url = link_at (self, cell);
        if (url && self->host && self->host->open_url)
            self->host->open_url (self->host_data, url);
    }
}

/* Ghostty's context-menu right click: select the word unless the click is
 * already inside the selection. */
static void
right_press (CorralTermSession *self, GhosttyPointCoordinate cell, const GhosttyGridRef *ref)
{
    GhosttyTerminalSelectWordOptions options = GHOSTTY_INIT_SIZED (GhosttyTerminalSelectWordOptions);
    GhosttyPoint point = { .tag = GHOSTTY_POINT_TAG_VIEWPORT, .value.coordinate = cell };
    GhosttySelection selection;
    bool contains = false;

    if (get_selection (self, &selection)
        && ghostty_terminal_selection_contains (self->terminal, &selection, point, &contains) == GHOSTTY_SUCCESS
        && contains)
        return;
    options.ref = *ref;
    if (ghostty_terminal_select_word (self->terminal, &options, &selection) != GHOSTTY_SUCCESS)
        return;
    set_selection (self, &selection);
    copy_on_select (self);
    g_signal_emit (self, signals[SIGNAL_CHANGED], 0);
}

static void
handle_button (CorralTermSession *self, const Input *input)
{
    GhosttyMouseButton button = mouse_button (input->button);
    guint bit = 1u << MIN (input->button, 31);
    GhosttyPointCoordinate cell = cell_at (self, input->x, input->y);
    GhosttyGridRef ref;

    self->mouse_x = input->x;
    self->mouse_y = input->y;
    self->mouse_mods = input->mods;
    if (input->pressed)
        self->buttons_down |= bit;
    else
        self->buttons_down &= ~bit;

    if (!ref_at (self, cell, &ref))
        return;

    if (button == GHOSTTY_MOUSE_BUTTON_LEFT && !input->pressed)
        left_release (self, &ref, cell, input->mods);

    /* Shift held selects even when the child tracks the mouse. */
    if (mouse_tracking (self) && !(input->mods & GHOSTTY_MODS_SHIFT)) {
        set_selection (self, NULL);
        ghostty_selection_gesture_reset (self->gesture, self->terminal);
        report_mouse (self, input->pressed ? GHOSTTY_MOUSE_ACTION_PRESS : GHOSTTY_MOUSE_ACTION_RELEASE,
                      button, input->x, input->y, input->mods);
        return;
    }
    if (!input->pressed)
        return;

    if (button == GHOSTTY_MOUSE_BUTTON_LEFT) {
        if (!(input->mods & GHOSTTY_MODS_SHIFT) || !extend_selection (self, input, &ref))
            left_press (self, input, &ref);
    } else if (button == GHOSTTY_MOUSE_BUTTON_MIDDLE) {
        request_paste (self, CORRAL_TERM_CLIPBOARD_PRIMARY);
    } else if (button == GHOSTTY_MOUSE_BUTTON_RIGHT) {
        right_press (self, cell, &ref);
    }
}

static void
handle_motion (CorralTermSession *self, const Input *input)
{
    GhosttyPointCoordinate cell = cell_at (self, input->x, input->y);
    GhosttyGridRef ref;

    self->mouse_x = input->x;
    self->mouse_y = input->y;
    self->mouse_mods = input->mods;

    if (mouse_tracking (self) && !(input->mods & GHOSTTY_MODS_SHIFT)) {
        report_mouse (self, GHOSTTY_MOUSE_ACTION_MOTION, held_button (self->buttons_down),
                      input->x, input->y, input->mods);
        return;
    }
    if (!(self->buttons_down & (1u << 1)) || !ref_at (self, cell, &ref))
        return;
    drag_to (self, &ref, input->x, input->y, input->mods);
}

/* Whole rows to move for an offset, carrying the remainder, the way
 * Ghostty 1.4's Surface.scrollCallback does. */
static long
scroll_rows (double *pending, double offset, double cell)
{
    double total = *pending + offset;
    double amount;

    if (fabs (total) < cell) {
        *pending = total;
        return 0;
    }
    amount = total / cell;
    *pending = total - amount * cell;
    return (long) trunc (amount);
}

static void
handle_scroll (CorralTermSession *self, const Input *input)
{
    double cell_height = MAX (self->cell_height, 1);
    double cell_width = MAX (self->cell_width, 1);
    long dy = 0;
    long dx = 0;
    gboolean tracking = mouse_tracking (self);
    GhosttyTerminalScreen screen = GHOSTTY_TERMINAL_SCREEN_PRIMARY;

    if (input->dy != 0) {
        double offset = input->precise ? input->dy * self->appearance.mouse_scroll_precision
                                       : input->dy * cell_height * self->appearance.mouse_scroll_discrete;
        dy = scroll_rows (&self->pending_scroll_y, offset, cell_height);
    }
    if (input->dx != 0)
        dx = input->precise ? scroll_rows (&self->pending_scroll_x, input->dx, cell_width)
                            : (long) round (input->dx);

    if (tracking)
        set_selection (self, NULL);

    ghostty_terminal_get (self->terminal, GHOSTTY_TERMINAL_DATA_ACTIVE_SCREEN, &screen);
    if (screen == GHOSTTY_TERMINAL_SCREEN_ALTERNATE && !tracking
        && mode_on (self, GHOSTTY_MODE_MOUSE_ALTERNATE_SCROLL)) {
        gboolean app = mode_on (self, GHOSTTY_MODE_CURSOR_KEYS);
        const char *seq = dy > 0 ? (app ? "\033OA" : "\033[A") : (app ? "\033OB" : "\033[B");
        for (long i = 0; i < labs (dy); i++)
            queue_output (self, (const guint8 *) seq, 3);
        return;
    }

    if (tracking) {
        for (long i = 0; i < labs (dy); i++)
            report_mouse (self, GHOSTTY_MOUSE_ACTION_PRESS,
                          dy > 0 ? GHOSTTY_MOUSE_BUTTON_FOUR : GHOSTTY_MOUSE_BUTTON_FIVE,
                          self->mouse_x, self->mouse_y, input->mods);
        for (long i = 0; i < labs (dx); i++)
            report_mouse (self, GHOSTTY_MOUSE_ACTION_PRESS,
                          dx > 0 ? GHOSTTY_MOUSE_BUTTON_SIX : GHOSTTY_MOUSE_BUTTON_SEVEN,
                          self->mouse_x, self->mouse_y, input->mods);
        return;
    }

    if (dy != 0) {
        GhosttyTerminalScrollViewport delta = {
            .tag = GHOSTTY_TERMINAL_SCROLL_VIEWPORT_DELTA,
            .value.delta = -dy,
        };
        ghostty_terminal_scroll_viewport (self->terminal, delta);
        g_signal_emit (self, signals[SIGNAL_CHANGED], 0);
    }
}

static void
handle_focus (CorralTermSession *self, gboolean focused)
{
    uint8_t buf[8];
    size_t written = 0;

    if (!mode_on (self, GHOSTTY_MODE_FOCUS_EVENT))
        return;
    if (ghostty_focus_encode (focused ? GHOSTTY_FOCUS_GAINED : GHOSTTY_FOCUS_LOST, buf, sizeof buf, &written)
        == GHOSTTY_SUCCESS)
        queue_output (self, buf, written);
}

static gboolean
handle_input (CorralTermSession *self, const Input *input)
{
    switch (input->kind) {
    case INPUT_KEY:
        return handle_key (self, &input->key);
    case INPUT_BUTTON:
        handle_button (self, input);
        break;
    case INPUT_MOTION:
        handle_motion (self, input);
        break;
    case INPUT_SCROLL:
        handle_scroll (self, input);
        break;
    case INPUT_FOCUS:
        handle_focus (self, input->pressed);
        break;
    case INPUT_PASTE:
        handle_paste (self, input->clipboard, input->text);
        break;
    }
    return TRUE;
}

static gboolean
submit (CorralTermSession *self, const Input *input)
{
    if (defer_input (self, input))
        return TRUE;
    return handle_input (self, input);
}

gboolean
corral_term_session_key (CorralTermSession *self, const CorralTermKey *key)
{
    Input input = { .kind = INPUT_KEY };

    g_return_val_if_fail (CORRAL_IS_TERM_SESSION (self), FALSE);
    input.key = *key;
    return submit (self, &input);
}

void
corral_term_session_mouse_button (CorralTermSession *self,
                                  gboolean pressed,
                                  guint button,
                                  double x,
                                  double y,
                                  GhosttyMods mods,
                                  gint64 time_us)
{
    Input input = {
        .kind = INPUT_BUTTON,
        .pressed = pressed,
        .button = button,
        .x = x,
        .y = y,
        .mods = mods,
        .time_us = time_us,
    };

    g_return_if_fail (CORRAL_IS_TERM_SESSION (self));
    submit (self, &input);
}

void
corral_term_session_mouse_motion (CorralTermSession *self, double x, double y, GhosttyMods mods)
{
    Input input = { .kind = INPUT_MOTION, .x = x, .y = y, .mods = mods };

    g_return_if_fail (CORRAL_IS_TERM_SESSION (self));
    submit (self, &input);
}

void
corral_term_session_scroll (CorralTermSession *self, double dx, double dy, gboolean precise, GhosttyMods mods)
{
    Input input = { .kind = INPUT_SCROLL, .dx = dx, .dy = dy, .precise = precise, .mods = mods };

    g_return_if_fail (CORRAL_IS_TERM_SESSION (self));
    submit (self, &input);
}

void
corral_term_session_focus (CorralTermSession *self, gboolean focused)
{
    Input input = { .kind = INPUT_FOCUS, .pressed = focused };

    g_return_if_fail (CORRAL_IS_TERM_SESSION (self));
    submit (self, &input);
}

void
corral_term_session_paste (CorralTermSession *self, CorralTermClipboard from, const char *text)
{
    Input input = { .kind = INPUT_PASTE, .clipboard = from, .text = (char *) text };

    g_return_if_fail (CORRAL_IS_TERM_SESSION (self));
    g_return_if_fail (text != NULL);
    submit (self, &input);
}

static void
reap (GPid pid, int status, gpointer user_data)
{
    g_spawn_close_pid (pid);
}

void
corral_term_session_close (CorralTermSession *self)
{
    g_return_if_fail (CORRAL_IS_TERM_SESSION (self));

    if (self->in_vt_write) {
        self->close_deferred = TRUE;
        return;
    }
    g_clear_handle_id (&self->read_source, g_source_remove);
    g_clear_handle_id (&self->write_source, g_source_remove);
    g_clear_handle_id (&self->autoscroll_source, g_source_remove);
    g_byte_array_set_size (self->outbound, 0);
    if (self->master >= 0) {
        close (self->master);
        self->master = -1;
    }
    if (self->child_watch) {
        g_clear_handle_id (&self->child_watch, g_source_remove);
        g_child_watch_add (self->pid, reap, NULL);
    }
}

void
corral_term_session_set_prefer_dark (CorralTermSession *self, gboolean dark)
{
    g_return_if_fail (CORRAL_IS_TERM_SESSION (self));
    self->prefer_dark = dark;
}

GhosttyTerminal
corral_term_session_get_terminal (CorralTermSession *self)
{
    g_return_val_if_fail (CORRAL_IS_TERM_SESSION (self), NULL);
    return self->terminal;
}

GPid
corral_term_session_get_pid (CorralTermSession *self)
{
    g_return_val_if_fail (CORRAL_IS_TERM_SESSION (self), 0);
    return self->pid;
}

gboolean
corral_term_session_get_render_held (CorralTermSession *self)
{
    g_return_val_if_fail (CORRAL_IS_TERM_SESSION (self), FALSE);
    return self->render_held;
}

gsize
corral_term_session_get_pending_output (CorralTermSession *self)
{
    g_return_val_if_fail (CORRAL_IS_TERM_SESSION (self), 0);
    return self->outbound->len;
}

const CorralAppearance *
corral_term_session_get_appearance (CorralTermSession *self)
{
    g_return_val_if_fail (CORRAL_IS_TERM_SESSION (self), NULL);
    return &self->appearance;
}

static void
corral_term_session_dispose (GObject *object)
{
    corral_term_session_close (CORRAL_TERM_SESSION (object));
    G_OBJECT_CLASS (corral_term_session_parent_class)->dispose (object);
}

static void
corral_term_session_finalize (GObject *object)
{
    CorralTermSession *self = CORRAL_TERM_SESSION (object);

    g_queue_clear_full (&self->deferred, (GDestroyNotify) input_free);
    ghostty_selection_gesture_free (self->gesture, self->terminal);
    ghostty_mouse_event_free (self->mouse_event);
    ghostty_mouse_encoder_free (self->mouse_encoder);
    ghostty_key_event_free (self->key_event);
    ghostty_key_encoder_free (self->key_encoder);
    if (self->terminal)
        ghostty_terminal_free (self->terminal);
    g_byte_array_unref (self->outbound);
    corral_appearance_clear (&self->appearance);
    G_OBJECT_CLASS (corral_term_session_parent_class)->finalize (object);
}

static void
corral_term_session_class_init (CorralTermSessionClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    object_class->dispose = corral_term_session_dispose;
    object_class->finalize = corral_term_session_finalize;

    signals[SIGNAL_CHANGED] = g_signal_new ("changed", G_TYPE_FROM_CLASS (klass),
                                            G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                            G_TYPE_NONE, 0);
    signals[SIGNAL_TITLE_CHANGED] = g_signal_new ("title-changed", G_TYPE_FROM_CLASS (klass),
                                                  G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                                  G_TYPE_NONE, 1, G_TYPE_STRING);
    signals[SIGNAL_BELL] = g_signal_new ("bell", G_TYPE_FROM_CLASS (klass),
                                         G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                         G_TYPE_NONE, 0);
    signals[SIGNAL_EXITED] = g_signal_new ("exited", G_TYPE_FROM_CLASS (klass),
                                           G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                           G_TYPE_NONE, 1, G_TYPE_INT);
}

static void
corral_term_session_init (CorralTermSession *self)
{
    self->master = -1;
    self->outbound = g_byte_array_new ();
    self->clipboard_timeout_ms = CLIPBOARD_TIMEOUT_MS;
    self->mouse_x = -1;
    self->mouse_y = -1;
    g_queue_init (&self->deferred);
}
