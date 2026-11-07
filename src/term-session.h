#pragma once

#include "config-appearance.h"

#include <ghostty/vt.h>
#include <glib-object.h>

G_BEGIN_DECLS

#define CORRAL_TYPE_TERM_SESSION (corral_term_session_get_type ())
G_DECLARE_FINAL_TYPE (CorralTermSession, corral_term_session, CORRAL, TERM_SESSION, GObject)

/* A pty, the child on it, and the libghostty-vt terminal fed by it. All of
 * it runs on the thread-default main context the session was created on.
 *
 * Signals:
 *   changed ()              terminal state changed; repaint
 *   title-changed (string)  OSC 0/2
 *   bell ()
 *   exited (int status)     the child exited and the exit line is drawn
 */
CorralTermSession *corral_term_session_new (const CorralAppearance *appearance,
                                            guint16 cols,
                                            guint16 rows,
                                            guint32 cell_width,
                                            guint32 cell_height);

/* Start ARGV[0] directly on a new pty, without a shell. */
gboolean corral_term_session_spawn (CorralTermSession *self,
                                    const char *const *argv,
                                    const char *cwd,
                                    GError **error);

/* Resize the grid and the pty window. Never starts another child. */
void corral_term_session_resize (CorralTermSession *self,
                                 guint16 cols,
                                 guint16 rows,
                                 guint32 cell_width,
                                 guint32 cell_height);

/* Queue bytes for the child. Never blocks. */
void corral_term_session_write (CorralTermSession *self, const void *data, gsize len);

/* Close the pty master. The child sees a hangup; nothing is signalled. */
void corral_term_session_close (CorralTermSession *self);

void corral_term_session_set_prefer_dark (CorralTermSession *self, gboolean dark);

GhosttyTerminal corral_term_session_get_terminal (CorralTermSession *self);
GPid corral_term_session_get_pid (CorralTermSession *self);
gboolean corral_term_session_get_render_held (CorralTermSession *self);
gsize corral_term_session_get_pending_output (CorralTermSession *self);
const CorralAppearance *corral_term_session_get_appearance (CorralTermSession *self);

/* The environment the child starts with, derived from PARENT_ENV. */
char **corral_term_session_child_env (const CorralAppearance *appearance,
                                      char **parent_env);

G_END_DECLS
