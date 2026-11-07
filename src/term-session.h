#pragma once

#include "config-appearance.h"

#include <ghostty/vt.h>
#include <glib-object.h>

G_BEGIN_DECLS

#define CORRAL_TYPE_TERM_SESSION (corral_term_session_get_type ())
G_DECLARE_FINAL_TYPE (CorralTermSession, corral_term_session, CORRAL, TERM_SESSION, GObject)

typedef enum {
    CORRAL_TERM_CLIPBOARD_STANDARD,
    CORRAL_TERM_CLIPBOARD_PRIMARY,
} CorralTermClipboard;

/* TEXT is NULL when the clipboard could not be read. */
typedef void (*CorralTermTextFn) (const char *text, gpointer data);

/* What the session asks of the window. read_text must call DONE exactly
 * once, from the main loop, possibly before it returns. */
typedef struct {
    void (*write_text) (gpointer host, CorralTermClipboard which, const char *text);
    void (*read_text) (gpointer host, CorralTermClipboard which, CorralTermTextFn done, gpointer data);
    void (*open_url) (gpointer host, const char *url);
} CorralTermHost;

typedef struct {
    GhosttyKeyAction action;
    /* XKB hardware keycode, as GDK reports it. */
    guint keycode;
    GhosttyMods mods;
    GhosttyMods consumed_mods;
    guint32 unshifted_codepoint;
    /* UTF-8 the key produced for the layout, or NULL. */
    const char *text;
} CorralTermKey;

/* A pty, the child on it, and the libghostty-vt terminal fed by it. All of
 * it runs on the default main context.
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

/* Pixels between the widget edge and the grid. Mouse positions are in the
 * same pixels as the cell size. */
void corral_term_session_set_padding (CorralTermSession *self,
                                      guint left,
                                      guint top,
                                      guint right,
                                      guint bottom);

/* Queue bytes for the child. Never blocks. */
void corral_term_session_write (CorralTermSession *self, const void *data, gsize len);

void corral_term_session_set_host (CorralTermSession *self,
                                   const CorralTermHost *host,
                                   gpointer host_data);

/* How long an OSC 52 read waits for the clipboard before replying empty. */
void corral_term_session_set_clipboard_timeout (CorralTermSession *self, guint ms);

/* TRUE when the key was written to the child or taken by a host chord. */
gboolean corral_term_session_key (CorralTermSession *self, const CorralTermKey *key);

/* BUTTON uses GDK numbering. TIME_US is a monotonic event time. */
void corral_term_session_mouse_button (CorralTermSession *self,
                                       gboolean pressed,
                                       guint button,
                                       double x,
                                       double y,
                                       GhosttyMods mods,
                                       gint64 time_us);
void corral_term_session_mouse_motion (CorralTermSession *self, double x, double y, GhosttyMods mods);

/* Offsets follow Ghostty: positive DY is up. Precise offsets are pixels,
 * others are wheel notches. */
void corral_term_session_scroll (CorralTermSession *self,
                                 double dx,
                                 double dy,
                                 gboolean precise,
                                 GhosttyMods mods);

void corral_term_session_focus (CorralTermSession *self, gboolean focused);

/* Paste TEXT the user asked for, framed for the terminal's paste mode. */
void corral_term_session_paste (CorralTermSession *self, CorralTermClipboard from, const char *text);

/* The selected text as copy writes it, or NULL. */
char *corral_term_session_selection_text (CorralTermSession *self);

/* TRUE while libghostty-vt is mid-write and the render state must not be
 * read. Input given meanwhile runs once the write returns. */
gboolean corral_term_session_is_busy (CorralTermSession *self);

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
