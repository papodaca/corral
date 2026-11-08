#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef struct {
    int cell_width;
    int cell_height;
    int box_thickness;
} CorralSpriteMetrics;

/* TRUE for the codepoints Ghostty 1.4 draws itself instead of taking them
 * from the font: box drawing, block elements, braille, and the geometric
 * powerline glyphs. */
gboolean corral_sprite_covers (guint32 cp);

/* Draw CP into the cell whose top-left corner is the current origin. */
void corral_sprite_draw (GtkSnapshot *snapshot,
                         guint32 cp,
                         const CorralSpriteMetrics *metrics,
                         const GdkRGBA *color);

G_END_DECLS
