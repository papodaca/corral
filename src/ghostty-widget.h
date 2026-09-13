#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define CORRAL_TYPE_GHOSTTY (corral_ghostty_get_type ())
G_DECLARE_FINAL_TYPE (CorralGhostty, corral_ghostty, CORRAL, GHOSTTY, GtkGLArea)

CorralGhostty *corral_ghostty_new (void);
const char *corral_ghostty_get_command (CorralGhostty *self);
void corral_ghostty_set_command (CorralGhostty *self, const char *command);
const char *corral_ghostty_get_cwd (CorralGhostty *self);
void corral_ghostty_set_cwd (CorralGhostty *self, const char *cwd);
float corral_ghostty_get_font_size (CorralGhostty *self);
void corral_ghostty_set_font_size (CorralGhostty *self, float font_size);
gboolean corral_ghostty_start (CorralGhostty *self, GError **error);

G_END_DECLS
