#define _GNU_SOURCE

#include "term-session.h"

#include <errno.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <glib-unix.h>
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

struct _CorralTermSession {
    GObject parent_instance;

    CorralAppearance appearance;
    GhosttyTerminal terminal;
    guint16 cols;
    guint16 rows;
    guint32 cell_width;
    guint32 cell_height;
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

static void
feed (CorralTermSession *self, const guint8 *data, gsize len)
{
    g_return_if_fail (!self->in_vt_write);
    self->in_vt_write = TRUE;
    ghostty_terminal_vt_write (self->terminal, data, len);
    self->in_vt_write = FALSE;
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
    apply_appearance (self);

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
    CorralTermSession *self = user_data;
    ReadResult result = read_chunk (self);

    if (result == READ_DATA)
        g_signal_emit (self, signals[SIGNAL_CHANGED], 0);
    if (result != READ_END)
        return G_SOURCE_CONTINUE;
    self->read_source = 0;
    return G_SOURCE_REMOVE;
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
    ghostty_terminal_resize (self->terminal, cols, rows, cell_width, cell_height);
    set_winsize (self);
    g_signal_emit (self, signals[SIGNAL_CHANGED], 0);
}

void
corral_term_session_write (CorralTermSession *self, const void *data, gsize len)
{
    g_return_if_fail (CORRAL_IS_TERM_SESSION (self));
    queue_output (self, data, len);
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

    g_clear_handle_id (&self->read_source, g_source_remove);
    g_clear_handle_id (&self->write_source, g_source_remove);
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
}
