#define _GNU_SOURCE

#include "term-session.h"

#include <errno.h>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    gboolean exited;
    int status;
    char *title;
} Events;

static void
on_exited (CorralTermSession *session, int status, Events *events)
{
    events->exited = TRUE;
    events->status = status;
}

static void
on_title (CorralTermSession *session, const char *title, Events *events)
{
    g_free (events->title);
    events->title = g_strdup (title);
}

static gboolean
time_up (gpointer user_data)
{
    *(gboolean *) user_data = TRUE;
    return G_SOURCE_REMOVE;
}

/* Run the default main context until *FLAG is set. */
static void
wait_for (gboolean *flag)
{
    gboolean expired = FALSE;
    guint timer = g_timeout_add_seconds (10, time_up, &expired);

    while (!*flag && !expired)
        g_main_context_iteration (NULL, TRUE);
    if (!expired)
        g_source_remove (timer);
    g_assert_false (expired);
}

static CorralTermSession *
new_session (Events *events)
{
    CorralTermSession *session = corral_term_session_new (NULL, 80, 24, 8, 16);

    g_signal_connect (session, "exited", G_CALLBACK (on_exited), events);
    g_signal_connect (session, "title-changed", G_CALLBACK (on_title), events);
    return session;
}

static char *
screen_text (CorralTermSession *session)
{
    GhosttyFormatter formatter = NULL;
    uint8_t *buf = NULL;
    size_t len = 0;
    char *text;

    g_assert_cmpint (ghostty_formatter_terminal_new (NULL, &formatter,
                                                     corral_term_session_get_terminal (session)),
                     ==, GHOSTTY_SUCCESS);
    g_assert_cmpint (ghostty_formatter_format_alloc (formatter, NULL, &buf, &len), ==, GHOSTTY_SUCCESS);
    text = g_strndup ((const char *) buf, len);
    ghostty_free (NULL, buf, len);
    ghostty_formatter_free (formatter);
    return text;
}

static char *
run_to_file (const char *script, const char *extra_arg)
{
    g_autofree char *dir = g_dir_make_tmp ("corral-term-session-XXXXXX", NULL);
    g_autofree char *path = g_build_filename (dir, "out", NULL);
    const char *argv[] = { "sh", "-c", script, "sh", path, extra_arg, NULL };
    Events events = { 0 };
    g_autoptr (CorralTermSession) session = new_session (&events);
    g_autoptr (GError) error = NULL;
    char *contents = NULL;

    g_assert_true (corral_term_session_spawn (session, argv, dir, &error));
    g_assert_no_error (error);
    wait_for (&events.exited);
    g_assert_cmpint (events.status, ==, 0);
    g_assert_true (g_file_get_contents (path, &contents, NULL, NULL));
    g_unlink (path);
    g_rmdir (dir);
    g_free (events.title);
    return contents;
}

static void
test_prints_and_exits (void)
{
    const char *argv[] = { "printf", "\033]2;corral-test\007hi", NULL };
    Events events = { 0 };
    g_autoptr (CorralTermSession) session = new_session (&events);
    g_autoptr (GError) error = NULL;
    g_autofree char *text = NULL;

    g_assert_true (corral_term_session_spawn (session, argv, NULL, &error));
    g_assert_no_error (error);
    wait_for (&events.exited);

    g_assert_cmpint (events.status, ==, 0);
    g_assert_cmpstr (events.title, ==, "corral-test");
    text = screen_text (session);
    g_assert_true (g_str_has_prefix (text, "hi\n"));
    g_assert_nonnull (strstr (text, "Process exited. Press any key to close the terminal."));
    g_free (events.title);
}

static void
test_exit_status (void)
{
    const char *argv[] = { "sh", "-c", "exit 3", NULL };
    Events events = { 0 };
    g_autoptr (CorralTermSession) session = new_session (&events);

    g_assert_true (corral_term_session_spawn (session, argv, NULL, NULL));
    wait_for (&events.exited);
    g_assert_cmpint (events.status, ==, 3);
}

static char *
expand (const char *value, const char *exe_dir, const char *resources)
{
    g_autoptr (GString) out = g_string_new (value);

    g_string_replace (out, "@EXE_DIR@", exe_dir, 0);
    g_string_replace (out, "@RESOURCES_DIR@", resources, 0);
    if (out->str[0] == '/')
        return g_canonicalize_filename (out->str, "/");
    return g_string_free (g_steal_pointer (&out), FALSE);
}

static void
test_child_env_matches_embed (void)
{
    g_autofree char *fixture = NULL;
    g_autofree char *dumped = NULL;
    g_autofree char *exe = g_file_read_link ("/proc/self/exe", NULL);
    g_autofree char *exe_dir = g_path_get_dirname (exe);
    g_auto (GStrv) lines = NULL;
    g_auto (GStrv) child = NULL;
    const char *resources = "/opt/corral-test/share/ghostty";
    guint checked = 0;

    g_assert_true (g_file_get_contents (FIXTURES_DIR "/embed-child-env.txt", &fixture, NULL, NULL));

    g_setenv ("GHOSTTY_RESOURCES_DIR", resources, TRUE);
    g_setenv ("HERDR_ENV", "1", TRUE);
    g_setenv ("HERDR_PANE_ID", "7", TRUE);
    g_setenv ("HERDR_SOCKET_PATH", "/run/corral-test/herdr.sock", TRUE);
    g_setenv ("HERDR_CONFIG_PATH", "/etc/corral-test/herdr.toml", TRUE);
    g_setenv ("VTE_VERSION", "8000", TRUE);
    dumped = run_to_file ("env > \"$1\"", NULL);
    g_unsetenv ("HERDR_ENV");
    g_unsetenv ("HERDR_PANE_ID");
    g_unsetenv ("VTE_VERSION");

    child = g_strsplit (dumped, "\n", -1);
    lines = g_strsplit (fixture, "\n", -1);
    for (char **line = lines; *line; line++) {
        g_auto (GStrv) rule = NULL;
        g_autofree char *key = NULL;
        const char *eq;

        if (**line == '\0' || **line == '#' || **line == '(')
            continue;
        rule = g_strsplit (*line, " ", 2);
        g_assert_nonnull (rule[1]);
        eq = strchr (rule[1], '=');
        key = eq ? g_strndup (rule[1], eq - rule[1]) : g_strdup (rule[1]);
        const char *got = g_environ_getenv (child, key);

        if (g_str_equal (rule[0], "set")) {
            g_autofree char *want = expand (eq + 1, exe_dir, resources);
            g_autofree char *got_norm = got && got[0] == '/' ? g_canonicalize_filename (got, "/") : g_strdup (got);
            g_autofree char *pattern = g_strconcat ("^", want, "$", NULL);
            g_assert_nonnull (got);
            if (!g_regex_match_simple (pattern, got_norm, 0, 0))
                g_error ("%s: want %s, got %s", key, want, got_norm);
        } else if (g_str_equal (rule[0], "append")) {
            g_autofree char *want = expand (eq + 1, exe_dir, resources);
            g_assert_nonnull (got);
            g_assert_true (g_str_equal (got, want) || g_str_has_suffix (got, want));
        } else if (g_str_equal (rule[0], "keep")) {
            g_assert_cmpstr (got, ==, g_getenv (key));
        } else if (g_str_equal (rule[0], "unset")) {
            g_assert_null (got);
        } else {
            g_error ("unknown fixture rule %s", rule[0]);
        }
        checked++;
    }
    g_assert_cmpuint (checked, >=, 15);
    g_unsetenv ("HERDR_SOCKET_PATH");
    g_unsetenv ("HERDR_CONFIG_PATH");
    g_unsetenv ("GHOSTTY_RESOURCES_DIR");
}

static void
test_child_env_without_resources (void)
{
    CorralAppearance appearance;
    char *parent[] = { "PATH=/usr/bin", "GHOSTTY_LOG=stderr", NULL };
    g_auto (GStrv) env = NULL;

    corral_appearance_init (&appearance);
    g_ptr_array_add (appearance.env, g_strdup ("COLORTERM=24bit"));
    g_ptr_array_add (appearance.env, g_strdup ("PATH="));
    env = corral_term_session_child_env (&appearance, parent);

    g_assert_cmpstr (g_environ_getenv (env, "TERM"), ==, "xterm-256color");
    g_assert_null (g_environ_getenv (env, "TERMINFO"));
    g_assert_null (g_environ_getenv (env, "GHOSTTY_LOG"));
    g_assert_cmpstr (g_environ_getenv (env, "COLORTERM"), ==, "24bit");
    g_assert_null (g_environ_getenv (env, "PATH"));
    corral_appearance_clear (&appearance);
}

static void
test_reply_goes_through_queue (void)
{
    g_autofree char *reply = run_to_file (
        "stty -echo -icanon; printf '\\033[6n'; IFS= read -r -d R r; printf '%s' \"$r\" > \"$1\"",
        NULL);

    g_assert_cmpstr (reply, ==, "\033[1;1");
}

static GHashTable *
open_fds (GPid pid)
{
    g_autofree char *path = g_strdup_printf ("/proc/%d/fd", pid);
    g_autoptr (GDir) dir = g_dir_open (path, 0, NULL);
    GHashTable *fds = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    const char *name;

    g_assert_nonnull (dir);
    while ((name = g_dir_read_name (dir)))
        g_hash_table_add (fds, g_strdup (name));
    return fds;
}

static char *
fd_target (GPid pid, const char *fd)
{
    g_autofree char *path = g_strdup_printf ("/proc/%d/fd/%s", pid, fd);

    return g_file_read_link (path, NULL);
}

static void
test_close_hangs_up_child (void)
{
    const char *argv[] = { "sleep", "30", NULL };
    Events events = { 0 };
    CorralTermSession *session = new_session (&events);
    g_autoptr (GHashTable) fds = NULL;
    GHashTableIter iter;
    gpointer key;
    gboolean gone = FALSE;
    GPid pid;

    g_assert_true (corral_term_session_spawn (session, argv, NULL, NULL));
    pid = corral_term_session_get_pid (session);
    g_assert_cmpint (pid, >, 0);

    fds = open_fds (pid);
    for (int fd = 0; fd <= 2; fd++) {
        g_autofree char *name = g_strdup_printf ("%d", fd);
        g_autofree char *target = fd_target (pid, name);
        g_assert_true (g_str_has_prefix (target, "/dev/pts/"));
    }
    g_hash_table_iter_init (&iter, fds);
    while (g_hash_table_iter_next (&iter, &key, NULL)) {
        g_autofree char *target = fd_target (pid, key);
        if (atoi (key) <= 2 || !target)
            continue;
        if (g_str_has_prefix (target, "socket:") || g_str_has_prefix (target, "pipe:")
            || g_str_has_prefix (target, "/dev/pts") || g_str_has_prefix (target, "/dev/ptmx"))
            g_error ("child inherited fd %s -> %s", (const char *) key, target);
    }

    g_object_unref (session);
    for (int i = 0; i < 500 && !gone; i++) {
        while (g_main_context_iteration (NULL, FALSE))
            ;
        gone = kill (pid, 0) != 0 && errno == ESRCH;
        if (!gone)
            g_usleep (10000);
    }
    g_assert_true (gone);
    g_assert_false (events.exited);
}

static void
test_exec_failure (void)
{
    const char *argv[] = { "/nonexistent/corral-test/herdr", NULL };
    Events events = { 0 };
    g_autoptr (CorralTermSession) session = new_session (&events);
    g_autoptr (GError) error = NULL;

    g_assert_false (corral_term_session_spawn (session, argv, NULL, &error));
    g_assert_nonnull (error);
    g_assert_cmpint (corral_term_session_get_pid (session), ==, 0);
    while (g_main_context_iteration (NULL, FALSE))
        ;
    g_assert_false (events.exited);
}

static void
test_resize_keeps_child (void)
{
    g_autofree char *dir = g_dir_make_tmp ("corral-term-session-XXXXXX", NULL);
    g_autofree char *path = g_build_filename (dir, "size", NULL);
    g_autofree char *ready = g_build_filename (dir, "ready", NULL);
    const char *argv[] = { "sh", "-c",
                           "trap 'stty size > \"$1\"; exit 0' WINCH; : > \"$2\"; while :; do sleep 0.05; done",
                           "sh", path, ready, NULL };
    Events events = { 0 };
    g_autoptr (CorralTermSession) session = new_session (&events);
    g_autofree char *size = NULL;
    GhosttyTerminal terminal = corral_term_session_get_terminal (session);
    guint16 cols = 0;
    guint16 rows = 0;
    GPid pid;

    g_assert_true (corral_term_session_spawn (session, argv, NULL, NULL));
    pid = corral_term_session_get_pid (session);
    for (int i = 0; i < 500 && !g_file_test (ready, G_FILE_TEST_EXISTS); i++) {
        while (g_main_context_iteration (NULL, FALSE))
            ;
        g_usleep (10000);
    }

    corral_term_session_resize (session, 120, 40, 16, 32);
    g_assert_cmpint (corral_term_session_get_pid (session), ==, pid);
    ghostty_terminal_get (terminal, GHOSTTY_TERMINAL_DATA_COLS, &cols);
    ghostty_terminal_get (terminal, GHOSTTY_TERMINAL_DATA_ROWS, &rows);
    g_assert_cmpuint (cols, ==, 120);
    g_assert_cmpuint (rows, ==, 40);

    wait_for (&events.exited);
    g_assert_true (g_file_get_contents (path, &size, NULL, NULL));
    g_assert_cmpstr (size, ==, "40 120\n");
    g_unlink (path);
    g_unlink (ready);
    g_rmdir (dir);
}

static void
test_appearance_applied (void)
{
    CorralAppearance appearance;
    g_autoptr (CorralTermSession) session = NULL;
    size_t bytes = 0;

    corral_appearance_init (&appearance);
    session = corral_term_session_new (&appearance, 80, 24, 8, 16);
    corral_appearance_clear (&appearance);

    g_assert_cmpint (ghostty_terminal_get (corral_term_session_get_terminal (session),
                                           GHOSTTY_TERMINAL_DATA_SCROLLBACK_MAX_BYTES, &bytes),
                     ==, GHOSTTY_SUCCESS);
    g_assert_cmpuint (bytes, ==, 50000000);
    g_assert_cmpstr (corral_term_session_get_appearance (session)->term, ==, "xterm-ghostty");
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/term-session/prints-and-exits", test_prints_and_exits);
    g_test_add_func ("/term-session/exit-status", test_exit_status);
    g_test_add_func ("/term-session/child-env-matches-embed", test_child_env_matches_embed);
    g_test_add_func ("/term-session/child-env-without-resources", test_child_env_without_resources);
    g_test_add_func ("/term-session/reply-goes-through-queue", test_reply_goes_through_queue);
    g_test_add_func ("/term-session/close-hangs-up-child", test_close_hangs_up_child);
    g_test_add_func ("/term-session/exec-failure", test_exec_failure);
    g_test_add_func ("/term-session/resize-keeps-child", test_resize_keeps_child);
    g_test_add_func ("/term-session/appearance-applied", test_appearance_applied);

    return g_test_run ();
}
