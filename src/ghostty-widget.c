#include "ghostty-widget.h"

#include "config-appearance.h"
#include "term-session.h"
#include "term-sprites.h"

#include <adwaita.h>
#include <ghostty/vt.h>
#include <hb-ot.h>
#include <math.h>
#include <string.h>

/* Ghostty 1.4's cursor blink interval and synchronized-output timeout. */
#define BLINK_MS 600
#define HOLD_TIMEOUT_US (1000 * G_TIME_SPAN_MILLISECOND)
#define KITTY_PLACEHOLDER 0x10EEEE

typedef struct {
    guint32 image_id;
    guint64 generation;
    GhosttyImageLayer layer;
    int32_t z;
    int32_t col;
    int32_t row;
    uint32_t offset_x;
    uint32_t offset_y;
    uint32_t width;
    uint32_t height;
    uint32_t src_x;
    uint32_t src_y;
    uint32_t src_width;
    uint32_t src_height;
    GdkTexture *texture;
} Placement;

typedef struct {
    gboolean visible;
    GhosttyCursorStyle style;
    gboolean blinking;
    int x;
    int y;
    int cells;
    char *text;
    gboolean bold;
    gboolean italic;
    GhosttyColorRgb color;
    GhosttyColorRgb text_color;
} Cursor;

struct _CorralGhostty {
    GtkWidget parent_instance;

    char *command;
    char *cwd;
    char *font_family;
    float font_size;
    gboolean started;

    CorralAppearance appearance;
    CorralTermSession *session;
    GhosttyRenderState render;
    GhosttyRenderStateRowIterator rows;
    GhosttyRenderStateRowCells cells;
    GhosttyRenderStateImageIterator images;

    PangoContext *pango;
    PangoFontDescription *font;
    int scale;
    int cell_width;
    int cell_height;
    int baseline;
    int underline_position;
    int underline_thickness;
    int strikethrough_position;
    int strikethrough_thickness;

    guint padding_left;
    guint padding_top;
    guint padding_right;
    guint padding_bottom;
    guint16 cols;
    guint16 rows_n;

    GhosttyRenderStateColors colors;
    GPtrArray *row_bg;
    GPtrArray *row_fg;
    gboolean rebuild_all;
    GArray *placements;
    guint64 placements_generation;
    gboolean have_placements;
    Cursor cursor;

    gboolean focused;
    gboolean blink_on;
    guint blink_source;
    gint64 hold_since;
    guint hold_source;
    gboolean precision_scroll;
    GhosttyMouseShape mouse_shape;
};

enum {
    PROP_0,
    PROP_COMMAND,
    PROP_CWD,
    PROP_FONT_FAMILY,
    PROP_FONT_SIZE,
    N_PROPS
};

enum {
    SIGNAL_EXITED,
    SIGNAL_TITLE_CHANGED,
    N_SIGNALS
};

static GParamSpec *properties[N_PROPS];
static guint signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE (CorralGhostty, corral_ghostty, GTK_TYPE_WIDGET)

static GdkRGBA
rgba (GhosttyColorRgb c, double alpha)
{
    return (GdkRGBA) { c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, alpha };
}

static gboolean
rgb_equal (GhosttyColorRgb a, GhosttyColorRgb b)
{
    return a.r == b.r && a.g == b.g && a.b == b.b;
}

/* ---- Fonts ---------------------------------------------------------- */

static gboolean
font_family_resolves (GtkWidget *widget, const char *wanted)
{
    PangoContext *ctx = gtk_widget_get_pango_context (widget);
    PangoFontMap *map = pango_context_get_font_map (ctx);
    PangoFontDescription *desc = pango_font_description_new ();
    pango_font_description_set_family (desc, wanted);
    pango_font_description_set_size (desc, 12 * PANGO_SCALE);
    PangoFont *font = pango_font_map_load_font (map, ctx, desc);
    pango_font_description_free (desc);
    if (font == NULL) {
        return FALSE;
    }
    PangoFontDescription *got = pango_font_describe (font);
    const char *family = pango_font_description_get_family (got);
    gboolean ok = family != NULL && g_ascii_strcasecmp (family, wanted) == 0;
    pango_font_description_free (got);
    g_object_unref (font);
    return ok;
}

static void
append_font_family (GString *families, GtkWidget *widget, GHashTable *seen, const char *family)
{
    if (family == NULL || family[0] == '\0' || strchr (family, ',') != NULL) {
        return;
    }
    char *key = g_ascii_strdown (family, -1);
    if (g_hash_table_contains (seen, key) || !font_family_resolves (widget, family)) {
        g_free (key);
        return;
    }
    g_hash_table_add (seen, key);
    g_string_append (families, family);
    g_string_append_c (families, ',');
}

static char *
desktop_monospace_family (void)
{
    GSettingsSchemaSource *source = g_settings_schema_source_get_default ();
    if (source == NULL) {
        return NULL;
    }
    GSettingsSchema *schema = g_settings_schema_source_lookup (
        source, "org.gnome.desktop.interface", TRUE);
    if (schema == NULL) {
        return NULL;
    }
    if (!g_settings_schema_has_key (schema, "monospace-font-name")) {
        g_settings_schema_unref (schema);
        return NULL;
    }
    g_settings_schema_unref (schema);
    GSettings *settings = g_settings_new ("org.gnome.desktop.interface");
    char *value = g_settings_get_string (settings, "monospace-font-name");
    g_object_unref (settings);
    if (value == NULL || value[0] == '\0') {
        g_free (value);
        return NULL;
    }
    PangoFontDescription *desc = pango_font_description_from_string (value);
    g_free (value);
    if (desc == NULL) {
        return NULL;
    }
    const char *family = pango_font_description_get_family (desc);
    char *copy = family != NULL ? g_strdup (family) : NULL;
    pango_font_description_free (desc);
    return copy;
}

/* The settings family first, then the fallbacks Corral has always used,
 * ending in fontconfig's monospace. */
static char *
font_families (CorralGhostty *self)
{
    static const char *candidates[] = {
        "Adwaita Mono",
        "Noto Sans Mono",
        "DejaVu Sans Mono",
        "Liberation Mono",
        "Source Code Pro",
        "JetBrains Mono",
    };
    GtkWidget *widget = GTK_WIDGET (self);
    GString *families = g_string_new (NULL);
    GHashTable *seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

    append_font_family (families, widget, seen, self->font_family);
    append_font_family (families, widget, seen, "Fira Code");
    char *desktop = desktop_monospace_family ();
    append_font_family (families, widget, seen, desktop);
    g_free (desktop);
    for (gsize i = 0; i < G_N_ELEMENTS (candidates); i++) {
        append_font_family (families, widget, seen, candidates[i]);
    }
    g_hash_table_unref (seen);
    g_string_append (families, "monospace");
    return g_string_free (families, FALSE);
}

static double
ot_metric (hb_font_t *font, hb_ot_metrics_tag_t tag, gboolean *found)
{
    hb_position_t value = 0;
    *found = hb_ot_metrics_get_position (font, tag, &value);
    return value;
}

/* Ghostty 1.4's cell metrics (font/Metrics.zig calc and the FreeType face's
 * getMetrics), from the font tables at FreeType's whole-pixel em size. */
static void
load_font (CorralGhostty *self)
{
    GtkWidget *widget = GTK_WIDGET (self);
    int scale = MAX (gtk_widget_get_scale_factor (widget), 1);
    int points_26dot6 = (int) round ((self->font_size > 0 ? self->font_size : 10.0f) * 64);
    int dpi = 96 * scale;
    /* FT_Set_Char_Size: the scaled size, then y_ppem rounded to a pixel. */
    int scaled_26dot6 = points_26dot6 * dpi / 72;
    double px_size = scaled_26dot6 / 64.0;
    double ppem = (scaled_26dot6 + 32) >> 6;

    g_clear_object (&self->pango);
    g_clear_pointer (&self->font, pango_font_description_free);
    self->scale = scale;
    self->pango = gtk_widget_create_pango_context (widget);
    char *families = font_families (self);
    self->font = pango_font_description_new ();
    pango_font_description_set_family (self->font, families);
    pango_font_description_set_absolute_size (self->font, px_size * PANGO_SCALE);
    g_free (families);
    pango_context_set_font_description (self->pango, self->font);

    PangoFont *font = pango_context_load_font (self->pango, self->font);
    hb_face_t *face = font != NULL ? hb_font_get_face (pango_font_get_hb_font (font)) : hb_face_get_empty ();
    unsigned upem = MAX (hb_face_get_upem (face), 1);
    hb_font_t *units = hb_font_create (face);
    hb_font_set_scale (units, upem, upem);
    double px_per_unit = ppem / upem;
    gboolean found;

    double ascent = ot_metric (units, HB_OT_METRICS_TAG_HORIZONTAL_ASCENDER, &found) * px_per_unit;
    double descent = ot_metric (units, HB_OT_METRICS_TAG_HORIZONTAL_DESCENDER, &found) * px_per_unit;
    double line_gap = ot_metric (units, HB_OT_METRICS_TAG_HORIZONTAL_LINE_GAP, &found) * px_per_unit;
    double ul_pos = ot_metric (units, HB_OT_METRICS_TAG_UNDERLINE_OFFSET, &found) * px_per_unit;
    gboolean has_ul_pos = found;
    double ul_thick = ot_metric (units, HB_OT_METRICS_TAG_UNDERLINE_SIZE, &found) * px_per_unit;
    double st_pos = ot_metric (units, HB_OT_METRICS_TAG_STRIKEOUT_OFFSET, &found) * px_per_unit;
    gboolean has_st_pos = found;
    double st_thick = ot_metric (units, HB_OT_METRICS_TAG_STRIKEOUT_SIZE, &found) * px_per_unit;
    double cap_height = ot_metric (units, HB_OT_METRICS_TAG_CAP_HEIGHT, &found) * px_per_unit;
    double ex_height = ot_metric (units, HB_OT_METRICS_TAG_X_HEIGHT, &found) * px_per_unit;

    /* The widest printable ASCII advance, hinted to whole pixels. */
    double face_width = 0;
    for (hb_codepoint_t c = ' '; c < 127; c++) {
        hb_codepoint_t glyph;
        if (hb_font_get_nominal_glyph (units, c, &glyph)) {
            double advance = round (hb_font_get_glyph_h_advance (units, glyph) * px_size / upem);
            face_width = MAX (face_width, advance);
        }
    }
    if (face_width <= 0) {
        face_width = round (px_size * 0.6);
    }
    hb_font_destroy (units);
    g_clear_object (&font);

    if (cap_height <= 0) {
        cap_height = 0.75 * ascent;
    }
    if (ex_height <= 0) {
        ex_height = 0.75 * cap_height;
    }
    if (ul_thick <= 0) {
        ul_thick = 0.15 * ex_height;
    }
    if (st_thick <= 0) {
        st_thick = ul_thick;
    }
    if (!has_ul_pos) {
        ul_pos = -ul_thick;
    }
    if (!has_st_pos) {
        st_pos = (ex_height + st_thick) * 0.5;
    }

    double face_height = ascent - descent + line_gap;
    double cell_width = round (face_width);
    double cell_height = round (face_height);
    double face_baseline = line_gap / 2 - descent;
    double cell_baseline = round (face_baseline - (cell_height - face_height) / 2);

    const CorralAppearance *a = &self->appearance;
    if (a->adjust_cell_width != 0) {
        cell_width += a->adjust_cell_width_percent ? round (cell_width * a->adjust_cell_width / 100.0)
                                                   : round (a->adjust_cell_width * scale);
    }
    if (a->adjust_cell_height != 0) {
        double before = cell_height;
        cell_height += a->adjust_cell_height_percent ? round (cell_height * a->adjust_cell_height / 100.0)
                                                     : round (a->adjust_cell_height * scale);
        cell_baseline += round ((cell_height - before) / 2);
    }
    double top_to_baseline = cell_height - cell_baseline;

    self->cell_width = MAX ((int) cell_width, 1);
    self->cell_height = MAX ((int) cell_height, 1);
    self->baseline = (int) top_to_baseline;
    self->underline_thickness = MAX (1, (int) ceil (ul_thick));
    self->strikethrough_thickness = MAX (1, (int) ceil (st_thick));
    self->underline_position = (int) round (top_to_baseline - ul_pos);
    self->strikethrough_position = (int) round (top_to_baseline - st_pos);
}

/* ---- Layout --------------------------------------------------------- */

static guint
padding_px (double points, int scale)
{
    return (guint) floor (points * 96.0 * scale / 72.0);
}

static void
free_node (gpointer node)
{
    /* An empty row has no snapshot node. GPtrArray still frees that slot. */
    if (node != NULL) {
        gsk_render_node_unref (node);
    }
}

static void
clear_rows (CorralGhostty *self)
{
    g_ptr_array_set_size (self->row_bg, 0);
    g_ptr_array_set_size (self->row_fg, 0);
    self->rebuild_all = TRUE;
}

static void
layout_grid (CorralGhostty *self)
{
    GtkWidget *widget = GTK_WIDGET (self);
    int scale = self->scale;
    int width = gtk_widget_get_width (widget) * scale;
    int height = gtk_widget_get_height (widget) * scale;
    const CorralAppearance *a = &self->appearance;
    guint left = padding_px (a->padding_left, scale);
    guint right = padding_px (a->padding_right, scale);
    guint top = padding_px (a->padding_top, scale);
    guint bottom = padding_px (a->padding_bottom, scale);

    if (width < 1 || height < 1) {
        return;
    }
    int cols = MAX ((width - (int) (left + right)) / self->cell_width, 1);
    int rows = MAX ((height - (int) (top + bottom)) / self->cell_height, 1);
    if (a->padding_balance != CORRAL_PADDING_BALANCE_FALSE) {
        int spare_x = width - cols * self->cell_width;
        int spare_y = height - rows * self->cell_height;
        left = MAX ((int) left, spare_x / 2);
        top = MAX ((int) top, spare_y / 2);
    }
    self->padding_left = left;
    self->padding_top = top;
    self->padding_right = MAX (width - (int) left - cols * self->cell_width, 0);
    self->padding_bottom = MAX (height - (int) top - rows * self->cell_height, 0);

    gboolean changed = cols != self->cols || rows != self->rows_n;
    self->cols = cols;
    self->rows_n = rows;
    if (self->session != NULL) {
        corral_term_session_set_padding (self->session, self->padding_left, self->padding_top,
                                         self->padding_right, self->padding_bottom);
        corral_term_session_resize (self->session, cols, rows, self->cell_width, self->cell_height);
    }
    if (changed) {
        clear_rows (self);
    }
}

static void
apply_font (CorralGhostty *self)
{
    load_font (self);
    clear_rows (self);
    layout_grid (self);
    gtk_widget_queue_draw (GTK_WIDGET (self));
}

/* ---- Images --------------------------------------------------------- */

static bool
decode_png (void *userdata, const GhosttyAllocator *allocator, const uint8_t *data, size_t len, GhosttySysImage *out)
{
    GBytes *bytes = g_bytes_new (data, len);
    GdkTexture *texture = gdk_texture_new_from_bytes (bytes, NULL);
    g_bytes_unref (bytes);
    if (texture == NULL) {
        return false;
    }
    int width = gdk_texture_get_width (texture);
    int height = gdk_texture_get_height (texture);
    size_t size = (size_t) width * height * 4;
    uint8_t *pixels = ghostty_alloc (allocator, size);
    if (pixels == NULL) {
        g_object_unref (texture);
        return false;
    }
    GdkTextureDownloader *downloader = gdk_texture_downloader_new (texture);
    gdk_texture_downloader_set_format (downloader, GDK_MEMORY_R8G8B8A8);
    gdk_texture_downloader_download_into (downloader, pixels, (gsize) width * 4);
    gdk_texture_downloader_free (downloader);
    g_object_unref (texture);
    out->width = width;
    out->height = height;
    out->data = pixels;
    out->data_len = size;
    return true;
}

static GdkTexture *
image_texture (GhosttyKittyGraphics graphics, guint32 image_id)
{
    GhosttyKittyGraphicsImage image = ghostty_kitty_graphics_image (graphics, image_id);
    uint32_t width = 0, height = 0;
    GhosttyKittyImageFormat format = 0;
    GhosttyString pixels = { 0 };

    if (image == NULL ||
        ghostty_kitty_graphics_image_get_multi (
            image, 4,
            (GhosttyKittyGraphicsImageData[]) {
                GHOSTTY_KITTY_GRAPHICS_IMAGE_DATA_WIDTH,
                GHOSTTY_KITTY_GRAPHICS_IMAGE_DATA_HEIGHT,
                GHOSTTY_KITTY_GRAPHICS_IMAGE_DATA_FORMAT,
                GHOSTTY_KITTY_GRAPHICS_IMAGE_DATA_PIXELS,
            },
            (void *[]) { &width, &height, &format, &pixels }, NULL) != GHOSTTY_SUCCESS ||
        width == 0 || height == 0 || pixels.ptr == NULL) {
        return NULL;
    }
    GdkMemoryFormat memory;
    gsize bpp;
    switch (format) {
    case GHOSTTY_KITTY_IMAGE_FORMAT_RGB: memory = GDK_MEMORY_R8G8B8; bpp = 3; break;
    case GHOSTTY_KITTY_IMAGE_FORMAT_RGBA: memory = GDK_MEMORY_R8G8B8A8; bpp = 4; break;
    case GHOSTTY_KITTY_IMAGE_FORMAT_GRAY_ALPHA: memory = GDK_MEMORY_G8A8; bpp = 2; break;
    case GHOSTTY_KITTY_IMAGE_FORMAT_GRAY: memory = GDK_MEMORY_G8; bpp = 1; break;
    default: return NULL;
    }
    if (pixels.len < (gsize) width * height * bpp) {
        return NULL;
    }
    GBytes *bytes = g_bytes_new (pixels.ptr, (gsize) width * height * bpp);
    GdkTexture *texture = gdk_memory_texture_new (width, height, memory, bytes, width * bpp);
    g_bytes_unref (bytes);
    return texture;
}

static void
clear_placement (gpointer data)
{
    Placement *p = data;
    g_clear_object (&p->texture);
}

static int
compare_placement (gconstpointer a, gconstpointer b)
{
    const Placement *pa = a, *pb = b;
    if (pa->layer != pb->layer) {
        return pa->layer < pb->layer ? -1 : 1;
    }
    return pa->z < pb->z ? -1 : pa->z > pb->z;
}

static void
update_placements (CorralGhostty *self, GhosttyTerminal terminal)
{
    guint64 generation = 0;
    ghostty_render_state_get (self->render, GHOSTTY_RENDER_STATE_DATA_IMAGE_PLACEMENTS_GENERATION, &generation);
    if (self->have_placements && generation == self->placements_generation) {
        return;
    }
    self->placements_generation = generation;
    self->have_placements = TRUE;

    GArray *old = self->placements;
    self->placements = g_array_new (FALSE, TRUE, sizeof (Placement));
    g_array_set_clear_func (self->placements, clear_placement);

    GhosttyKittyGraphics graphics = NULL;
    if (ghostty_render_state_get (self->render, GHOSTTY_RENDER_STATE_DATA_IMAGE_ITERATOR, &self->images) != GHOSTTY_SUCCESS ||
        ghostty_terminal_get (terminal, GHOSTTY_TERMINAL_DATA_KITTY_GRAPHICS, &graphics) != GHOSTTY_SUCCESS ||
        graphics == NULL) {
        g_array_unref (old);
        return;
    }
    while (ghostty_render_state_image_iterator_next (self->images)) {
        GhosttyImageProtocol protocol;
        Placement p = { 0 };
        ghostty_render_state_image_get (self->images, GHOSTTY_RENDER_STATE_IMAGE_DATA_PROTOCOL, &protocol);
        if (protocol != GHOSTTY_IMAGE_PROTOCOL_KITTY) {
            continue;
        }
        GhosttyRenderStateImageData keys[] = {
            GHOSTTY_RENDER_STATE_IMAGE_DATA_IMAGE_ID,
            GHOSTTY_RENDER_STATE_IMAGE_DATA_IMAGE_GENERATION,
            GHOSTTY_RENDER_STATE_IMAGE_DATA_LAYER,
            GHOSTTY_RENDER_STATE_IMAGE_DATA_Z,
            GHOSTTY_RENDER_STATE_IMAGE_DATA_VIEWPORT_COL,
            GHOSTTY_RENDER_STATE_IMAGE_DATA_VIEWPORT_ROW,
            GHOSTTY_RENDER_STATE_IMAGE_DATA_CELL_OFFSET_X,
            GHOSTTY_RENDER_STATE_IMAGE_DATA_CELL_OFFSET_Y,
            GHOSTTY_RENDER_STATE_IMAGE_DATA_PIXEL_WIDTH,
            GHOSTTY_RENDER_STATE_IMAGE_DATA_PIXEL_HEIGHT,
            GHOSTTY_RENDER_STATE_IMAGE_DATA_SOURCE_X,
            GHOSTTY_RENDER_STATE_IMAGE_DATA_SOURCE_Y,
            GHOSTTY_RENDER_STATE_IMAGE_DATA_SOURCE_WIDTH,
            GHOSTTY_RENDER_STATE_IMAGE_DATA_SOURCE_HEIGHT,
        };
        void *values[] = {
            &p.image_id, &p.generation, &p.layer, &p.z, &p.col, &p.row, &p.offset_x, &p.offset_y,
            &p.width, &p.height, &p.src_x, &p.src_y, &p.src_width, &p.src_height,
        };
        if (ghostty_render_state_image_get_multi (self->images, G_N_ELEMENTS (keys), keys, values, NULL) != GHOSTTY_SUCCESS ||
            p.width == 0 || p.height == 0 || p.src_width == 0 || p.src_height == 0) {
            continue;
        }
        for (guint i = 0; old != NULL && i < old->len; i++) {
            Placement *prev = &g_array_index (old, Placement, i);
            if (prev->image_id == p.image_id && prev->generation == p.generation && prev->texture != NULL) {
                p.texture = g_object_ref (prev->texture);
                break;
            }
        }
        if (p.texture == NULL) {
            p.texture = image_texture (graphics, p.image_id);
        }
        if (p.texture != NULL) {
            g_array_append_val (self->placements, p);
        }
    }
    g_array_sort (self->placements, compare_placement);
    if (old != NULL) {
        g_array_unref (old);
    }
}

static void
draw_placements (CorralGhostty *self, GtkSnapshot *snapshot, GhosttyImageLayer layer)
{
    for (guint i = 0; self->placements != NULL && i < self->placements->len; i++) {
        Placement *p = &g_array_index (self->placements, Placement, i);
        if (p->layer != layer) {
            continue;
        }
        double x = self->padding_left + (double) p->col * self->cell_width + p->offset_x;
        double y = self->padding_top + (double) p->row * self->cell_height + p->offset_y;
        double sx = (double) p->width / p->src_width;
        double sy = (double) p->height / p->src_height;
        int tw = gdk_texture_get_width (p->texture);
        int th = gdk_texture_get_height (p->texture);
        gtk_snapshot_push_clip (snapshot, &GRAPHENE_RECT_INIT (x, y, p->width, p->height));
        gtk_snapshot_append_scaled_texture (snapshot, p->texture, GSK_SCALING_FILTER_LINEAR,
                                            &GRAPHENE_RECT_INIT (x - p->src_x * sx, y - p->src_y * sy, tw * sx, th * sy));
        gtk_snapshot_pop (snapshot);
    }
}

/* ---- Cells ---------------------------------------------------------- */

typedef struct {
    char text[64];
    gsize text_len;
    guint32 codepoint;
    GhosttyCellWide wide;
    GhosttyStyle style;
    GhosttyColorRgb fg;
    gboolean has_bg;
    GhosttyColorRgb bg;
    double alpha;
} CellPaint;

static GhosttyColorRgb
style_fg (CorralGhostty *self, const GhosttyStyle *style, gboolean has_fg, GhosttyColorRgb fg)
{
    const CorralAppearance *a = &self->appearance;
    if ((style->flags & GHOSTTY_STYLE_FLAG_BOLD) && a->bold_color_kind != CORRAL_BOLD_COLOR_UNSET) {
        if (a->bold_color_kind == CORRAL_BOLD_COLOR_RGB) {
            return a->bold_color;
        }
        if (style->fg_color.tag == GHOSTTY_STYLE_COLOR_PALETTE && style->fg_color.value.palette < 8) {
            return self->colors.palette[style->fg_color.value.palette + 8];
        }
    }
    return has_fg ? fg : self->colors.foreground;
}

static GhosttyColorRgb
selection_color (const CorralTermColor *config, GhosttyColorRgb fallback,
                 gboolean inverse, GhosttyColorRgb cell_fg, GhosttyColorRgb cell_bg)
{
    switch (config->kind) {
    case CORRAL_TERM_COLOR_RGB: return config->rgb;
    case CORRAL_TERM_COLOR_CELL_FOREGROUND: return inverse ? cell_bg : cell_fg;
    case CORRAL_TERM_COLOR_CELL_BACKGROUND: return inverse ? cell_fg : cell_bg;
    default: return fallback;
    }
}

/* Ghostty 1.4's rebuildCells color rules for one cell. */
static void
read_cell (CorralGhostty *self, GhosttyRenderStateRowCells cells, gboolean selected, CellPaint *out)
{
    const CorralAppearance *a = &self->appearance;
    GhosttyCell raw = 0;
    bool has_styling = false;
    GhosttyBuffer text = { (uint8_t *) out->text, sizeof out->text - 1, 0 };
    GhosttyColorRgb fg_raw, bg_raw;

    ghostty_render_state_row_cells_get (cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_RAW, &raw);
    out->wide = GHOSTTY_CELL_WIDE_NARROW;
    ghostty_cell_get (raw, GHOSTTY_CELL_DATA_WIDE, &out->wide);
    out->codepoint = 0;
    ghostty_cell_get (raw, GHOSTTY_CELL_DATA_CODEPOINT, &out->codepoint);
    if (ghostty_render_state_row_cells_get (cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_UTF8, &text) != GHOSTTY_SUCCESS) {
        text.len = 0;
    }
    out->text_len = text.len;
    out->text[text.len] = '\0';

    ghostty_style_default (&out->style);
    ghostty_render_state_row_cells_get (cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_HAS_STYLING, &has_styling);
    if (has_styling) {
        ghostty_render_state_row_cells_get (cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_STYLE, &out->style);
    }
    gboolean has_fg = ghostty_render_state_row_cells_get (cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_FG_COLOR, &fg_raw) == GHOSTTY_SUCCESS;
    gboolean has_bg_style = ghostty_render_state_row_cells_get (cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_BG_COLOR, &bg_raw) == GHOSTTY_SUCCESS;

    GhosttyColorRgb fg_style = style_fg (self, &out->style, has_fg, fg_raw);
    GhosttyColorRgb final_bg = has_bg_style ? bg_raw : self->colors.background;
    gboolean inverse = (out->style.flags & GHOSTTY_STYLE_FLAG_INVERSE) != 0;
    gboolean covering = out->codepoint == 0x2588;

    if (selected) {
        out->has_bg = TRUE;
        out->bg = selection_color (&a->selection_background, self->colors.foreground, inverse, fg_style, final_bg);
        out->fg = selection_color (&a->selection_foreground, self->colors.background, inverse, fg_style, final_bg);
    } else {
        out->has_bg = inverse || has_bg_style;
        out->bg = inverse != covering ? fg_style : final_bg;
        out->fg = inverse ? final_bg : fg_style;
    }
    out->alpha = (out->style.flags & GHOSTTY_STYLE_FLAG_FAINT) ? a->faint_opacity : 1.0;
}

typedef struct {
    PangoFontDescription *desc;
    GString *text;
    GArray *byte_col;
    int first_col;
} TextRun;

static void
emit_glyphs (GtkSnapshot *snapshot, PangoFont *font, PangoGlyphString *glyphs, int start, int end,
             const int *pen, const GdkRGBA *color, double baseline)
{
    if (end <= start) {
        return;
    }
    PangoGlyphString *part = pango_glyph_string_new ();
    pango_glyph_string_set_size (part, end - start);
    for (int i = start; i < end; i++) {
        PangoGlyphInfo info = glyphs->glyphs[i];
        info.geometry.width = i + 1 < end ? pen[i + 1] - pen[i] : glyphs->glyphs[i].geometry.width;
        part->glyphs[i - start] = info;
        part->log_clusters[i - start] = 0;
    }
    GskRenderNode *node = gsk_text_node_new (font, part, color,
                                             &GRAPHENE_POINT_INIT ((float) pen[start] / PANGO_SCALE, baseline));
    if (node != NULL) {
        gtk_snapshot_append_node (snapshot, node);
        gsk_render_node_unref (node);
    }
    pango_glyph_string_free (part);
}

/* Shape RUN and place each cluster on its column, so the grid never drifts
 * whatever the font's advances are. */
static void
flush_run (CorralGhostty *self, GtkSnapshot *snapshot, TextRun *run, const CellPaint *row,
           double x0, double baseline)
{
    if (run->text->len == 0) {
        return;
    }
    PangoAttrList *attrs = pango_attr_list_new ();
    pango_attr_list_insert (attrs, pango_attr_font_desc_new (run->desc));
    GList *items = pango_itemize (self->pango, run->text->str, 0, (int) run->text->len, attrs, NULL);
    pango_attr_list_unref (attrs);

    for (GList *l = items; l != NULL; l = l->next) {
        PangoItem *item = l->data;
        PangoGlyphString *glyphs = pango_glyph_string_new ();
        const char *item_text = run->text->str + item->offset;
        pango_shape_full (item_text, item->length, run->text->str, (int) run->text->len, &item->analysis, glyphs);

        int *pen = g_new (int, glyphs->num_glyphs + 1);
        int *col = g_new (int, glyphs->num_glyphs);
        int cluster = -1, cursor = 0;
        for (int i = 0; i < glyphs->num_glyphs; i++) {
            int byte = item->offset + glyphs->log_clusters[i];
            col[i] = g_array_index (run->byte_col, int, byte);
            if (glyphs->log_clusters[i] != cluster) {
                cluster = glyphs->log_clusters[i];
                cursor = (int) ((x0 + col[i] * self->cell_width) * PANGO_SCALE);
            }
            pen[i] = cursor;
            cursor += glyphs->glyphs[i].geometry.width;
        }

        int start = 0;
        for (int i = 1; i <= glyphs->num_glyphs; i++) {
            const CellPaint *a = &row[col[start]];
            if (i < glyphs->num_glyphs) {
                const CellPaint *b = &row[col[i]];
                if (rgb_equal (a->fg, b->fg) && a->alpha == b->alpha) {
                    continue;
                }
            }
            GdkRGBA color = rgba (a->fg, a->alpha);
            emit_glyphs (snapshot, item->analysis.font, glyphs, start, i, pen, &color, baseline);
            start = i;
        }
        g_free (pen);
        g_free (col);
        pango_glyph_string_free (glyphs);
    }
    g_list_free_full (items, (GDestroyNotify) pango_item_free);
    g_string_truncate (run->text, 0);
    g_array_set_size (run->byte_col, 0);
    g_clear_pointer (&run->desc, pango_font_description_free);
}

static PangoFontDescription *
cell_font (CorralGhostty *self, const GhosttyStyle *style)
{
    PangoFontDescription *desc = pango_font_description_copy_static (self->font);
    if (style->flags & GHOSTTY_STYLE_FLAG_BOLD) {
        pango_font_description_set_weight (desc, PANGO_WEIGHT_BOLD);
    }
    if (style->flags & GHOSTTY_STYLE_FLAG_ITALIC) {
        pango_font_description_set_style (desc, PANGO_STYLE_ITALIC);
    }
    return desc;
}

static void
draw_underline (CorralGhostty *self, GtkSnapshot *snapshot, int kind, double x, double y, int width, const GdkRGBA *color)
{
    int thick = self->underline_thickness;
    int limit = self->cell_height + self->cell_height / 4;
    int pos = self->underline_position;

    switch (kind) {
    case GHOSTTY_SGR_UNDERLINE_SINGLE:
        pos = MIN (pos, limit - thick);
        gtk_snapshot_append_color (snapshot, color, &GRAPHENE_RECT_INIT (x, y + pos, width, thick));
        break;
    case GHOSTTY_SGR_UNDERLINE_DOUBLE:
        pos = MIN (pos, limit - 2 * thick);
        gtk_snapshot_append_color (snapshot, color, &GRAPHENE_RECT_INIT (x, y + MAX (pos - thick, 0), width, thick));
        gtk_snapshot_append_color (snapshot, color, &GRAPHENE_RECT_INIT (x, y + pos + thick, width, thick));
        break;
    case GHOSTTY_SGR_UNDERLINE_DASHED: {
        pos = MIN (pos, limit - thick);
        int dash = width / 3 + 1;
        for (int i = 0; i < width / dash + 1; i += 2) {
            int dx = i * dash;
            gtk_snapshot_append_color (snapshot, color,
                                       &GRAPHENE_RECT_INIT (x + dx, y + pos, MIN (dash, width - dx), thick));
        }
        break;
    }
    case GHOSTTY_SGR_UNDERLINE_DOTTED: {
        double radius = G_SQRT2 / 2 * thick;
        double cy = MIN (pos + 0.5 * thick, limit - ceil (radius));
        double count = MAX (MIN (MIN (ceil (width / (4 * radius)), floor (width / (3 * radius))),
                                 floor (width / (2 * radius + 1))), 1.0);
        GskPathBuilder *b = gsk_path_builder_new ();
        for (int i = 0; i < (int) count; i++) {
            gsk_path_builder_add_circle (b, &GRAPHENE_POINT_INIT (x + (width / count) * (i + 0.5), y + cy), radius);
        }
        GskPath *path = gsk_path_builder_free_to_path (b);
        gtk_snapshot_append_fill (snapshot, path, GSK_FILL_RULE_WINDING, color);
        gsk_path_unref (path);
        break;
    }
    case GHOSTTY_SGR_UNDERLINE_CURLY: {
        double amplitude = width / G_PI;
        double top = MIN (pos, limit - amplitude - thick);
        double bottom = top + amplitude;
        double center = 0.5 * width, r = 0.4;
        GskPathBuilder *b = gsk_path_builder_new ();
        gsk_path_builder_move_to (b, x, y + bottom);
        gsk_path_builder_cubic_to (b, x + center * r, y + bottom, x + center - center * r, y + top, x + center, y + top);
        gsk_path_builder_cubic_to (b, x + center + center * r, y + top, x + width - center * r, y + bottom, x + width, y + bottom);
        GskPath *path = gsk_path_builder_free_to_path (b);
        GskStroke *stroke = gsk_stroke_new (thick);
        gsk_stroke_set_line_cap (stroke, GSK_LINE_CAP_ROUND);
        gtk_snapshot_append_stroke (snapshot, path, stroke, color);
        gsk_stroke_free (stroke);
        gsk_path_unref (path);
        break;
    }
    default:
        break;
    }
}

static void
build_row (CorralGhostty *self, int y, GskRenderNode **bg_out, GskRenderNode **fg_out)
{
    GtkSnapshot *bg = gtk_snapshot_new ();
    GtkSnapshot *fg = gtk_snapshot_new ();
    GhosttyRenderStateRowSelection sel = GHOSTTY_INIT_SIZED (GhosttyRenderStateRowSelection);
    gboolean has_sel = ghostty_render_state_row_get (self->rows, GHOSTTY_RENDER_STATE_ROW_DATA_SELECTION, &sel) == GHOSTTY_SUCCESS;
    CellPaint *row = g_new0 (CellPaint, self->cols);
    double top = self->padding_top + (double) y * self->cell_height;
    double left = self->padding_left;
    double baseline = top + self->baseline;
    CorralSpriteMetrics sprite = { self->cell_width, self->cell_height, self->underline_thickness };

    ghostty_render_state_row_get (self->rows, GHOSTTY_RENDER_STATE_ROW_DATA_CELLS, &self->cells);
    int ncols = 0;
    while (ncols < self->cols && ghostty_render_state_row_cells_next (self->cells)) {
        int x_compare = ncols;
        CellPaint *cell = &row[ncols];
        ghostty_render_state_row_cells_select (self->cells, ncols);
        GhosttyCell raw = 0;
        GhosttyCellWide wide = GHOSTTY_CELL_WIDE_NARROW;
        ghostty_render_state_row_cells_get (self->cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_RAW, &raw);
        ghostty_cell_get (raw, GHOSTTY_CELL_DATA_WIDE, &wide);
        if (wide == GHOSTTY_CELL_WIDE_SPACER_TAIL && x_compare > 0) {
            x_compare--;
        }
        gboolean selected = has_sel && x_compare >= sel.start_x && x_compare <= sel.end_x;
        read_cell (self, self->cells, selected, cell);
        ncols++;
    }

    /* Backgrounds, merged across cells of one color. */
    for (int x = 0; x < ncols;) {
        if (!row[x].has_bg) {
            x++;
            continue;
        }
        int end = x + 1;
        while (end < ncols && row[end].has_bg && rgb_equal (row[end].bg, row[x].bg)) {
            end++;
        }
        GdkRGBA color = rgba (row[x].bg, 1.0);
        gtk_snapshot_append_color (bg, &color,
                                   &GRAPHENE_RECT_INIT (left + x * self->cell_width, top,
                                                        (end - x) * self->cell_width, self->cell_height));
        x = end;
    }

    /* Decorations under the text, then text and sprites. */
    TextRun run = { NULL, g_string_new (NULL), g_array_new (FALSE, FALSE, sizeof (int)), 0 };
    for (int x = 0; x < ncols; x++) {
        CellPaint *cell = &row[x];
        GdkRGBA color = rgba (cell->fg, cell->alpha);
        double cx = left + x * self->cell_width;
        int cells = cell->wide == GHOSTTY_CELL_WIDE_WIDE ? 2 : 1;

        if (cell->style.flags & GHOSTTY_STYLE_FLAG_INVISIBLE) {
            flush_run (self, fg, &run, row, left, baseline);
            continue;
        }
        int underline = GHOSTTY_STYLE_UNDERLINE (cell->style);
        if (underline != GHOSTTY_SGR_UNDERLINE_NONE) {
            GdkRGBA ul = color;
            const GhosttyStyleColor *uc = &cell->style.underline_color;
            if (uc->tag == GHOSTTY_STYLE_COLOR_RGB) {
                ul = rgba (uc->value.rgb, cell->alpha);
            } else if (uc->tag == GHOSTTY_STYLE_COLOR_PALETTE) {
                ul = rgba (self->colors.palette[uc->value.palette], cell->alpha);
            }
            draw_underline (self, fg, underline, cx, top, self->cell_width, &ul);
        }
        if (cell->style.flags & GHOSTTY_STYLE_FLAG_OVERLINE) {
            gtk_snapshot_append_color (fg, &color, &GRAPHENE_RECT_INIT (cx, top, self->cell_width, self->underline_thickness));
        }

        gboolean sprite_cell = cell->text_len > 0 && corral_sprite_covers (cell->codepoint) &&
                               g_utf8_strlen (cell->text, cell->text_len) == 1;
        if (cell->text_len == 0 || cell->codepoint == KITTY_PLACEHOLDER || sprite_cell) {
            flush_run (self, fg, &run, row, left, baseline);
            if (sprite_cell) {
                gtk_snapshot_save (fg);
                gtk_snapshot_translate (fg, &GRAPHENE_POINT_INIT (cx, top));
                corral_sprite_draw (fg, cell->codepoint, &sprite, &color);
                gtk_snapshot_restore (fg);
            }
        } else {
            PangoFontDescription *desc = cell_font (self, &cell->style);
            if (run.desc != NULL && !pango_font_description_equal (run.desc, desc)) {
                flush_run (self, fg, &run, row, left, baseline);
            }
            if (run.desc == NULL) {
                run.desc = desc;
            } else {
                pango_font_description_free (desc);
            }
            for (gsize i = 0; i < cell->text_len; i++) {
                g_array_append_val (run.byte_col, x);
            }
            g_string_append_len (run.text, cell->text, cell->text_len);
        }
        if (cell->style.flags & GHOSTTY_STYLE_FLAG_STRIKETHROUGH) {
            gtk_snapshot_append_color (fg, &color,
                                       &GRAPHENE_RECT_INIT (cx, top + self->strikethrough_position,
                                                            cells * self->cell_width, self->strikethrough_thickness));
        }
    }
    flush_run (self, fg, &run, row, left, baseline);
    g_string_free (run.text, TRUE);
    g_array_unref (run.byte_col);
    g_free (row);

    *bg_out = gtk_snapshot_free_to_node (bg);
    *fg_out = gtk_snapshot_free_to_node (fg);
}

static void
read_cursor (CorralGhostty *self)
{
    const CorralAppearance *a = &self->appearance;
    GhosttyRenderStateCursor cursor = GHOSTTY_INIT_SIZED (GhosttyRenderStateCursor);
    Cursor *c = &self->cursor;

    g_clear_pointer (&c->text, g_free);
    c->visible = FALSE;
    if (ghostty_render_state_get (self->render, GHOSTTY_RENDER_STATE_DATA_CURSOR, &cursor) != GHOSTTY_SUCCESS ||
        !cursor.visible || !cursor.viewport_has_value) {
        return;
    }
    c->visible = TRUE;
    c->style = cursor.visual_style;
    c->blinking = cursor.blinking;
    c->x = cursor.viewport_x - (cursor.wide_tail && cursor.viewport_x > 0 ? 1 : 0);
    c->y = cursor.viewport_y;
    c->cells = cursor.wide_tail ? 2 : 1;
    c->bold = c->italic = FALSE;

    GhosttyColorRgb cell_fg = self->colors.foreground;
    GhosttyColorRgb cell_bg = self->colors.background;
    gboolean inverse = FALSE;
    if (ghostty_render_state_get (self->render, GHOSTTY_RENDER_STATE_DATA_ROW_ITERATOR, &self->rows) == GHOSTTY_SUCCESS) {
        for (int y = 0; ghostty_render_state_row_iterator_next (self->rows); y++) {
            if (y != c->y) {
                continue;
            }
            CellPaint cell;
            ghostty_render_state_row_get (self->rows, GHOSTTY_RENDER_STATE_ROW_DATA_CELLS, &self->cells);
            if (ghostty_render_state_row_cells_select (self->cells, c->x) == GHOSTTY_SUCCESS) {
                read_cell (self, self->cells, FALSE, &cell);
                inverse = (cell.style.flags & GHOSTTY_STYLE_FLAG_INVERSE) != 0;
                cell_fg = inverse ? cell.bg : cell.fg;
                cell_bg = inverse ? cell.fg : (cell.has_bg ? cell.bg : self->colors.background);
                if (cell.wide == GHOSTTY_CELL_WIDE_WIDE) {
                    c->cells = 2;
                }
                if (cell.text_len > 0 && cell.codepoint != KITTY_PLACEHOLDER && !corral_sprite_covers (cell.codepoint)) {
                    c->text = g_strndup (cell.text, cell.text_len);
                    c->bold = (cell.style.flags & GHOSTTY_STYLE_FLAG_BOLD) != 0;
                    c->italic = (cell.style.flags & GHOSTTY_STYLE_FLAG_ITALIC) != 0;
                }
            }
            break;
        }
    }

    if (self->colors.cursor_has_value) {
        c->color = self->colors.cursor;
    } else if (a->cursor_color.kind == CORRAL_TERM_COLOR_CELL_FOREGROUND) {
        c->color = inverse ? cell_bg : cell_fg;
    } else if (a->cursor_color.kind == CORRAL_TERM_COLOR_CELL_BACKGROUND) {
        c->color = inverse ? cell_fg : cell_bg;
    } else {
        c->color = self->colors.foreground;
    }
    switch (a->cursor_text.kind) {
    case CORRAL_TERM_COLOR_RGB: c->text_color = a->cursor_text.rgb; break;
    case CORRAL_TERM_COLOR_CELL_FOREGROUND: c->text_color = inverse ? cell_bg : cell_fg; break;
    case CORRAL_TERM_COLOR_CELL_BACKGROUND: c->text_color = inverse ? cell_fg : cell_bg; break;
    default: c->text_color = self->colors.background; break;
    }
}

static gboolean
on_blink (gpointer data)
{
    CorralGhostty *self = CORRAL_GHOSTTY (data);
    self->blink_on = !self->blink_on;
    gtk_widget_queue_draw (GTK_WIDGET (self));
    return G_SOURCE_CONTINUE;
}

static void
reset_blink (CorralGhostty *self)
{
    gboolean want = self->cursor.visible && self->cursor.blinking && self->focused;
    self->blink_on = TRUE;
    g_clear_handle_id (&self->blink_source, g_source_remove);
    if (want) {
        self->blink_source = g_timeout_add (BLINK_MS, on_blink, self);
    }
}

static void
update_frame (CorralGhostty *self)
{
    GhosttyTerminal terminal = corral_term_session_get_terminal (self->session);
    GhosttyRenderStateDirty dirty = GHOSTTY_RENDER_STATE_DIRTY_FULL;
    gboolean was_blinking = self->cursor.visible && self->cursor.blinking;

    if (ghostty_render_state_update (self->render, terminal) != GHOSTTY_SUCCESS) {
        return;
    }
    ghostty_render_state_get (self->render, GHOSTTY_RENDER_STATE_DATA_DIRTY, &dirty);
    GhosttyRenderStateColors colors = GHOSTTY_INIT_SIZED (GhosttyRenderStateColors);
    ghostty_render_state_get (self->render, GHOSTTY_RENDER_STATE_DATA_COLORS, &colors);
    if (memcmp (&colors, &self->colors, sizeof colors) != 0) {
        self->colors = colors;
        dirty = GHOSTTY_RENDER_STATE_DIRTY_FULL;
    }
    gboolean all = self->rebuild_all || dirty == GHOSTTY_RENDER_STATE_DIRTY_FULL || self->row_bg->len != self->rows_n;
    if (self->row_bg->len != self->rows_n) {
        g_ptr_array_set_size (self->row_bg, self->rows_n);
        g_ptr_array_set_size (self->row_fg, self->rows_n);
    }

    if (dirty != GHOSTTY_RENDER_STATE_DIRTY_FALSE || all) {
        ghostty_render_state_get (self->render, GHOSTTY_RENDER_STATE_DATA_ROW_ITERATOR, &self->rows);
        for (int y = 0; y < self->rows_n && ghostty_render_state_row_iterator_next (self->rows); y++) {
            bool row_dirty = true;
            ghostty_render_state_row_get (self->rows, GHOSTTY_RENDER_STATE_ROW_DATA_DIRTY, &row_dirty);
            if (!all && !row_dirty) {
                continue;
            }
            GskRenderNode *bg = NULL, *fg = NULL;
            build_row (self, y, &bg, &fg);
            g_clear_pointer (&self->row_bg->pdata[y], gsk_render_node_unref);
            g_clear_pointer (&self->row_fg->pdata[y], gsk_render_node_unref);
            self->row_bg->pdata[y] = bg;
            self->row_fg->pdata[y] = fg;
        }
    }
    read_cursor (self);
    update_placements (self, terminal);
    ghostty_render_state_clean (self->render);
    self->rebuild_all = FALSE;

    if (was_blinking != (self->cursor.visible && self->cursor.blinking)) {
        reset_blink (self);
    }
}

static void
draw_cursor (CorralGhostty *self, GtkSnapshot *snapshot)
{
    Cursor *c = &self->cursor;
    if (!c->visible || c->y >= self->rows_n) {
        return;
    }
    GhosttyCursorStyle style = self->focused ? c->style : GHOSTTY_CURSOR_STYLE_BLOCK_HOLLOW;
    if (self->focused && c->blinking && !self->blink_on) {
        return;
    }
    double x = self->padding_left + (double) c->x * self->cell_width;
    double y = self->padding_top + (double) c->y * self->cell_height;
    int width = c->cells * self->cell_width;
    int height = self->cell_height;
    GdkRGBA color = rgba (c->color, self->appearance.cursor_opacity);

    switch (style) {
    case GHOSTTY_CURSOR_STYLE_BLOCK:
        gtk_snapshot_append_color (snapshot, &color, &GRAPHENE_RECT_INIT (x, y, width, height));
        if (c->text != NULL) {
            GtkSnapshot *sub = gtk_snapshot_new ();
            CellPaint cell = { 0 };
            CellPaint row[2];
            TextRun run = { NULL, g_string_new (c->text), g_array_new (FALSE, FALSE, sizeof (int)), 0 };
            GhosttyStyle cell_style;
            ghostty_style_default (&cell_style);
            cell_style.flags = (c->bold ? GHOSTTY_STYLE_FLAG_BOLD : 0) | (c->italic ? GHOSTTY_STYLE_FLAG_ITALIC : 0);
            cell.fg = c->text_color;
            cell.alpha = 1.0;
            row[0] = row[1] = cell;
            run.desc = cell_font (self, &cell_style);
            int zero = 0;
            for (gsize i = 0; i < run.text->len; i++) {
                g_array_append_val (run.byte_col, zero);
            }
            flush_run (self, sub, &run, row, x, y + self->baseline);
            g_string_free (run.text, TRUE);
            g_array_unref (run.byte_col);
            GskRenderNode *node = gtk_snapshot_free_to_node (sub);
            if (node != NULL) {
                gtk_snapshot_push_clip (snapshot, &GRAPHENE_RECT_INIT (x, y, width, height));
                gtk_snapshot_append_node (snapshot, node);
                gtk_snapshot_pop (snapshot);
                gsk_render_node_unref (node);
            }
        }
        break;
    case GHOSTTY_CURSOR_STYLE_BLOCK_HOLLOW:
        gtk_snapshot_append_color (snapshot, &color, &GRAPHENE_RECT_INIT (x, y, width, 1));
        gtk_snapshot_append_color (snapshot, &color, &GRAPHENE_RECT_INIT (x, y + height - 1, width, 1));
        gtk_snapshot_append_color (snapshot, &color, &GRAPHENE_RECT_INIT (x, y + 1, 1, height - 2));
        gtk_snapshot_append_color (snapshot, &color, &GRAPHENE_RECT_INIT (x + width - 1, y + 1, 1, height - 2));
        break;
    case GHOSTTY_CURSOR_STYLE_BAR:
        gtk_snapshot_append_color (snapshot, &color, &GRAPHENE_RECT_INIT (x - 1, y, 1, height));
        break;
    case GHOSTTY_CURSOR_STYLE_UNDERLINE: {
        int pos = MIN (self->underline_position, height + height / 4 - self->underline_thickness);
        gtk_snapshot_append_color (snapshot, &color, &GRAPHENE_RECT_INIT (x, y + pos, width, 1));
        break;
    }
    default:
        break;
    }
}

static gboolean
on_hold_timeout (gpointer data)
{
    CorralGhostty *self = CORRAL_GHOSTTY (data);
    self->hold_source = 0;
    gtk_widget_queue_draw (GTK_WIDGET (self));
    return G_SOURCE_REMOVE;
}

/* A render hold keeps the last frame, for at most Ghostty's one second. */
static gboolean
frame_held (CorralGhostty *self)
{
    if (corral_term_session_is_busy (self->session)) {
        return TRUE;
    }
    if (!corral_term_session_get_render_held (self->session)) {
        self->hold_since = 0;
        g_clear_handle_id (&self->hold_source, g_source_remove);
        return FALSE;
    }
    gint64 now = g_get_monotonic_time ();
    if (self->hold_since == 0) {
        self->hold_since = now;
        self->hold_source = g_timeout_add (HOLD_TIMEOUT_US / G_TIME_SPAN_MILLISECOND, on_hold_timeout, self);
    }
    return now - self->hold_since < HOLD_TIMEOUT_US;
}

static void
corral_ghostty_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
    CorralGhostty *self = CORRAL_GHOSTTY (widget);
    int scale = self->scale;
    double width = gtk_widget_get_width (widget) * scale;
    double height = gtk_widget_get_height (widget) * scale;

    if (self->session != NULL && !frame_held (self)) {
        update_frame (self);
    }
    gtk_snapshot_save (snapshot);
    gtk_snapshot_scale (snapshot, 1.0 / scale, 1.0 / scale);
    GdkRGBA background = rgba (self->session != NULL ? self->colors.background : self->appearance.background, 1.0);
    gtk_snapshot_append_color (snapshot, &background, &GRAPHENE_RECT_INIT (0, 0, width, height));
    if (self->session != NULL) {
        draw_placements (self, snapshot, GHOSTTY_IMAGE_LAYER_BELOW_BACKGROUND);
        for (guint y = 0; y < self->row_bg->len; y++) {
            if (self->row_bg->pdata[y] != NULL) {
                gtk_snapshot_append_node (snapshot, self->row_bg->pdata[y]);
            }
        }
        draw_placements (self, snapshot, GHOSTTY_IMAGE_LAYER_BELOW_TEXT);
        for (guint y = 0; y < self->row_fg->len; y++) {
            if (self->row_fg->pdata[y] != NULL) {
                gtk_snapshot_append_node (snapshot, self->row_fg->pdata[y]);
            }
        }
        draw_placements (self, snapshot, GHOSTTY_IMAGE_LAYER_ABOVE_TEXT);
        draw_cursor (self, snapshot);
    }
    gtk_snapshot_restore (snapshot);
}

/* ---- Session -------------------------------------------------------- */

static void
host_write_text (gpointer host, CorralTermClipboard which, const char *text)
{
    GtkWidget *widget = GTK_WIDGET (host);
    GdkClipboard *clip = which == CORRAL_TERM_CLIPBOARD_PRIMARY ? gtk_widget_get_primary_clipboard (widget)
                                                                : gtk_widget_get_clipboard (widget);
    gdk_clipboard_set_text (clip, text);
}

typedef struct {
    CorralTermTextFn done;
    gpointer data;
} ReadText;

static void
on_read_text (GObject *source, GAsyncResult *result, gpointer user_data)
{
    ReadText *read = user_data;
    char *text = gdk_clipboard_read_text_finish (GDK_CLIPBOARD (source), result, NULL);
    read->done (text, read->data);
    g_free (text);
    g_free (read);
}

static void
host_read_text (gpointer host, CorralTermClipboard which, CorralTermTextFn done, gpointer data)
{
    GtkWidget *widget = GTK_WIDGET (host);
    GdkClipboard *clip = which == CORRAL_TERM_CLIPBOARD_PRIMARY ? gtk_widget_get_primary_clipboard (widget)
                                                                : gtk_widget_get_clipboard (widget);
    ReadText *read = g_new (ReadText, 1);
    read->done = done;
    read->data = data;
    gdk_clipboard_read_text_async (clip, NULL, on_read_text, read);
}

static void
host_open_url (gpointer host, const char *url)
{
    g_app_info_launch_default_for_uri_async (url, NULL, NULL, NULL, NULL);
}

static const CorralTermHost host = {
    .write_text = host_write_text,
    .read_text = host_read_text,
    .open_url = host_open_url,
};

static const char *
cursor_name (GhosttyMouseShape shape)
{
    switch (shape) {
    case GHOSTTY_MOUSE_SHAPE_DEFAULT: return "default";
    case GHOSTTY_MOUSE_SHAPE_CONTEXT_MENU: return "context-menu";
    case GHOSTTY_MOUSE_SHAPE_HELP: return "help";
    case GHOSTTY_MOUSE_SHAPE_POINTER: return "pointer";
    case GHOSTTY_MOUSE_SHAPE_PROGRESS: return "progress";
    case GHOSTTY_MOUSE_SHAPE_WAIT: return "wait";
    case GHOSTTY_MOUSE_SHAPE_CELL: return "cell";
    case GHOSTTY_MOUSE_SHAPE_CROSSHAIR: return "crosshair";
    case GHOSTTY_MOUSE_SHAPE_VERTICAL_TEXT: return "vertical-text";
    case GHOSTTY_MOUSE_SHAPE_ALIAS: return "alias";
    case GHOSTTY_MOUSE_SHAPE_COPY: return "copy";
    case GHOSTTY_MOUSE_SHAPE_MOVE: return "move";
    case GHOSTTY_MOUSE_SHAPE_NO_DROP: return "no-drop";
    case GHOSTTY_MOUSE_SHAPE_NOT_ALLOWED: return "not-allowed";
    case GHOSTTY_MOUSE_SHAPE_GRAB: return "grab";
    case GHOSTTY_MOUSE_SHAPE_GRABBING: return "grabbing";
    case GHOSTTY_MOUSE_SHAPE_ALL_SCROLL: return "all-scroll";
    case GHOSTTY_MOUSE_SHAPE_COL_RESIZE: return "col-resize";
    case GHOSTTY_MOUSE_SHAPE_ROW_RESIZE: return "row-resize";
    case GHOSTTY_MOUSE_SHAPE_N_RESIZE: return "n-resize";
    case GHOSTTY_MOUSE_SHAPE_E_RESIZE: return "e-resize";
    case GHOSTTY_MOUSE_SHAPE_S_RESIZE: return "s-resize";
    case GHOSTTY_MOUSE_SHAPE_W_RESIZE: return "w-resize";
    case GHOSTTY_MOUSE_SHAPE_NE_RESIZE: return "ne-resize";
    case GHOSTTY_MOUSE_SHAPE_NW_RESIZE: return "nw-resize";
    case GHOSTTY_MOUSE_SHAPE_SE_RESIZE: return "se-resize";
    case GHOSTTY_MOUSE_SHAPE_SW_RESIZE: return "sw-resize";
    case GHOSTTY_MOUSE_SHAPE_EW_RESIZE: return "ew-resize";
    case GHOSTTY_MOUSE_SHAPE_NS_RESIZE: return "ns-resize";
    case GHOSTTY_MOUSE_SHAPE_NESW_RESIZE: return "nesw-resize";
    case GHOSTTY_MOUSE_SHAPE_NWSE_RESIZE: return "nwse-resize";
    case GHOSTTY_MOUSE_SHAPE_ZOOM_IN: return "zoom-in";
    case GHOSTTY_MOUSE_SHAPE_ZOOM_OUT: return "zoom-out";
    default: return "text";
    }
}

static void
on_session_changed (CorralTermSession *session, gpointer user_data)
{
    CorralGhostty *self = CORRAL_GHOSTTY (user_data);
    GhosttyMouseShape shape = GHOSTTY_MOUSE_SHAPE_TEXT;

    if (!corral_term_session_is_busy (session) &&
        ghostty_terminal_get (corral_term_session_get_terminal (session), GHOSTTY_TERMINAL_DATA_MOUSE_SHAPE, &shape) == GHOSTTY_SUCCESS &&
        shape != self->mouse_shape) {
        self->mouse_shape = shape;
        gtk_widget_set_cursor_from_name (GTK_WIDGET (self), cursor_name (shape));
    }
    gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
on_session_title (CorralTermSession *session, const char *title, gpointer user_data)
{
    g_signal_emit (user_data, signals[SIGNAL_TITLE_CHANGED], 0, title);
}

static void
on_session_exited (CorralTermSession *session, int status, gpointer user_data)
{
    gtk_widget_queue_draw (GTK_WIDGET (user_data));
    g_signal_emit (user_data, signals[SIGNAL_EXITED], 0, status);
}

static void
strip_nested_herdr_env (void)
{
    /* Herdr refuses to start when HERDR_ENV=1 (already inside a Herdr pane).
     * Corral is a host window, not a nested client, so drop pane identity.
     * Keep socket/config so we still attach to a running server. */
    gchar **keys = g_listenv ();
    for (gchar **k = keys; k != NULL && *k != NULL; k++) {
        if (!g_str_has_prefix (*k, "HERDR_")) {
            continue;
        }
        if (g_strcmp0 (*k, "HERDR_SOCKET_PATH") == 0 ||
            g_strcmp0 (*k, "HERDR_CONFIG_PATH") == 0) {
            continue;
        }
        g_unsetenv (*k);
    }
    g_strfreev (keys);
}

static void
ensure_session (CorralGhostty *self)
{
    if (!self->started || self->session != NULL || self->cols == 0 || self->command == NULL) {
        return;
    }
    strip_nested_herdr_env ();
    self->session = corral_term_session_new (&self->appearance, self->cols, self->rows_n,
                                             self->cell_width, self->cell_height);
    corral_term_session_set_prefer_dark (self->session, adw_style_manager_get_dark (adw_style_manager_get_default ()));
    corral_term_session_set_padding (self->session, self->padding_left, self->padding_top,
                                     self->padding_right, self->padding_bottom);
    corral_term_session_set_host (self->session, &host, self);
    corral_term_session_focus (self->session, self->focused);
    g_signal_connect (self->session, "changed", G_CALLBACK (on_session_changed), self);
    g_signal_connect (self->session, "title-changed", G_CALLBACK (on_session_title), self);
    g_signal_connect (self->session, "exited", G_CALLBACK (on_session_exited), self);

    const char *argv[] = { self->command, NULL };
    GError *error = NULL;
    if (!corral_term_session_spawn (self->session, argv, self->cwd, &error)) {
        g_warning ("terminal session failed: %s", error->message);
        g_clear_error (&error);
    }
    clear_rows (self);
    gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
load_appearance (CorralGhostty *self)
{
    char **files = corral_appearance_default_files ();
    char **themes = corral_appearance_default_theme_dirs (CORRAL_THEMES_DIR);
    corral_appearance_load (&self->appearance, (const char *const *) files, (const char *const *) themes,
                            adw_style_manager_get_dark (adw_style_manager_get_default ()));
    g_strfreev (files);
    g_strfreev (themes);
}

/* ---- Input ---------------------------------------------------------- */

static GhosttyMods
mods_from_gdk (GdkModifierType state)
{
    GhosttyMods mods = 0;
    if (state & GDK_SHIFT_MASK) {
        mods |= GHOSTTY_MODS_SHIFT;
    }
    if (state & GDK_CONTROL_MASK) {
        mods |= GHOSTTY_MODS_CTRL;
    }
    if (state & GDK_ALT_MASK) {
        mods |= GHOSTTY_MODS_ALT;
    }
    if (state & GDK_SUPER_MASK) {
        mods |= GHOSTTY_MODS_SUPER;
    }
    if (state & GDK_LOCK_MASK) {
        mods |= GHOSTTY_MODS_CAPS_LOCK;
    }
    return mods;
}

static GhosttyMods
mods_from_controller (GtkEventController *controller, GdkModifierType state)
{
    GdkEvent *event = gtk_event_controller_get_current_event (controller);
    GdkDevice *device = NULL;
    if (event != NULL) {
        /* Event state can drop Control/Alt that XKB marked consumed.
         * The device still has them, which is what Ghostty GTK uses. */
        state = gdk_event_get_modifier_state (event);
        device = gdk_event_get_device (event);
        if (device != NULL) {
            state |= gdk_device_get_modifier_state (device);
        }
    }
    GhosttyMods mods = mods_from_gdk (state);
    if (device != NULL && gdk_device_get_num_lock_state (device)) {
        mods |= GHOSTTY_MODS_NUM_LOCK;
    }
    return mods;
}

static GdkEvent *
current_key_event (GtkEventController *controller)
{
    GdkEvent *event = gtk_event_controller_get_current_event (controller);
    if (event == NULL) {
        return NULL;
    }
    GdkEventType type = gdk_event_get_event_type (event);
    return type == GDK_KEY_PRESS || type == GDK_KEY_RELEASE ? event : NULL;
}

static guint32
unshifted_codepoint (GtkWidget *widget, GtkEventController *controller, guint keyval, guint keycode)
{
    GdkEvent *event = current_key_event (controller);
    guint32 result = 0;
    if (event != NULL) {
        guint layout = gdk_key_event_get_layout (event);
        GdkKeymapKey *keys = NULL;
        guint *keyvals = NULL;
        int n = 0;
        if (gdk_display_map_keycode (gtk_widget_get_display (widget), keycode, &keys, &keyvals, &n)) {
            for (int i = 0; i < n; i++) {
                if (keys[i].group == (int) layout && keys[i].level == 0) {
                    result = gdk_keyval_to_unicode (keyvals[i]);
                    break;
                }
            }
            g_free (keys);
            g_free (keyvals);
        }
    }
    return result != 0 ? result : gdk_keyval_to_unicode (gdk_keyval_to_lower (keyval));
}

static gboolean
send_key (CorralGhostty *self, GtkEventControllerKey *controller, GhosttyKeyAction action,
          guint keyval, guint keycode, GdkModifierType state)
{
    if (self->session == NULL) {
        return FALSE;
    }
    GtkEventController *ec = GTK_EVENT_CONTROLLER (controller);
    GdkEvent *event = current_key_event (ec);
    gunichar ch = gdk_keyval_to_unicode (keyval);
    char text[8] = { 0 };
    /* Control bytes belong to the encoder. Printable utf8 is still
     * passed, including with Ctrl held, matching Ghostty GTK. */
    if (action == GHOSTTY_KEY_ACTION_PRESS && ch >= 0x20) {
        g_unichar_to_utf8 (ch, text);
    }
    CorralTermKey key = {
        .action = action,
        .keycode = keycode,
        .mods = mods_from_controller (ec, state),
        .consumed_mods = event != NULL ? mods_from_gdk (gdk_key_event_get_consumed_modifiers (event)) : 0,
        .unshifted_codepoint = unshifted_codepoint (GTK_WIDGET (self), ec, keyval, keycode),
        .text = text[0] != 0 ? text : NULL,
    };
    if (action == GHOSTTY_KEY_ACTION_PRESS) {
        reset_blink (self);
    }
    return corral_term_session_key (self->session, &key);
}

static gboolean
on_key_pressed (GtkEventControllerKey *controller, guint keyval, guint keycode,
                GdkModifierType state, gpointer user_data)
{
    return send_key (CORRAL_GHOSTTY (user_data), controller, GHOSTTY_KEY_ACTION_PRESS, keyval, keycode, state);
}

static void
on_key_released (GtkEventControllerKey *controller, guint keyval, guint keycode,
                 GdkModifierType state, gpointer user_data)
{
    send_key (CORRAL_GHOSTTY (user_data), controller, GHOSTTY_KEY_ACTION_RELEASE, keyval, keycode, state);
}

/* Button, motion, and release events, including after the pointer leaves
 * the widget mid-drag. */
static gboolean
on_pointer_event (GtkEventControllerLegacy *controller, GdkEvent *event, gpointer user_data)
{
    CorralGhostty *self = CORRAL_GHOSTTY (user_data);
    GdkEventType type = gdk_event_get_event_type (event);
    double x, y;

    if (self->session == NULL ||
        (type != GDK_BUTTON_PRESS && type != GDK_BUTTON_RELEASE && type != GDK_MOTION_NOTIFY) ||
        !gdk_event_get_position (event, &x, &y)) {
        return FALSE;
    }
    GtkWidget *widget = GTK_WIDGET (self);
    GtkNative *native = gtk_widget_get_native (widget);
    graphene_point_t point;
    double nx, ny;
    gtk_native_get_surface_transform (native, &nx, &ny);
    if (!gtk_widget_compute_point (GTK_WIDGET (native), widget, &GRAPHENE_POINT_INIT (x - nx, y - ny), &point)) {
        return FALSE;
    }
    GhosttyMods mods = mods_from_controller (GTK_EVENT_CONTROLLER (controller), gdk_event_get_modifier_state (event));
    double px = point.x * self->scale, py = point.y * self->scale;

    if (type == GDK_MOTION_NOTIFY) {
        corral_term_session_mouse_motion (self->session, px, py, mods);
        return FALSE;
    }
    if (type == GDK_BUTTON_PRESS) {
        gtk_widget_grab_focus (widget);
    }
    corral_term_session_mouse_button (self->session, type == GDK_BUTTON_PRESS, gdk_button_event_get_button (event),
                                      px, py, mods, (gint64) gdk_event_get_time (event) * 1000);
    return TRUE;
}

static void
on_scroll_begin (GtkEventControllerScroll *controller, gpointer user_data)
{
    CORRAL_GHOSTTY (user_data)->precision_scroll = TRUE;
}

static void
on_scroll_end (GtkEventControllerScroll *controller, gpointer user_data)
{
    CORRAL_GHOSTTY (user_data)->precision_scroll = FALSE;
}

static gboolean
on_scroll (GtkEventControllerScroll *controller, double dx, double dy, gpointer user_data)
{
    CorralGhostty *self = CORRAL_GHOSTTY (user_data);
    if (self->session == NULL) {
        return FALSE;
    }
    GtkEventController *ec = GTK_EVENT_CONTROLLER (controller);
    GhosttyMods mods = mods_from_controller (ec, gtk_event_controller_get_current_event_state (ec));
    /* Ghostty: positive is up/right, the reverse of GTK, and its GTK
     * surface scales by the device scale and precise deltas by ten. */
    double multiplier = self->scale * (self->precision_scroll ? 10.0 : 1.0);
    corral_term_session_scroll (self->session, -dx * multiplier, -dy * multiplier, self->precision_scroll, mods);
    return TRUE;
}

static void
set_focus (CorralGhostty *self, gboolean focused)
{
    self->focused = focused;
    if (self->session != NULL) {
        corral_term_session_focus (self->session, focused);
    }
    reset_blink (self);
    gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
on_focus_enter (GtkEventControllerFocus *controller, gpointer user_data)
{
    set_focus (CORRAL_GHOSTTY (user_data), TRUE);
}

static void
on_focus_leave (GtkEventControllerFocus *controller, gpointer user_data)
{
    set_focus (CORRAL_GHOSTTY (user_data), FALSE);
}

/* ---- GtkWidget ------------------------------------------------------ */

static void
corral_ghostty_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
    CorralGhostty *self = CORRAL_GHOSTTY (widget);
    layout_grid (self);
    ensure_session (self);
}

static void
on_scale_factor (GObject *object, GParamSpec *pspec, gpointer user_data)
{
    CorralGhostty *self = CORRAL_GHOSTTY (object);
    if (gtk_widget_get_scale_factor (GTK_WIDGET (self)) != self->scale) {
        apply_font (self);
    }
}

static void
on_dark_changed (GObject *object, GParamSpec *pspec, gpointer user_data)
{
    CorralGhostty *self = CORRAL_GHOSTTY (user_data);
    if (self->session != NULL) {
        corral_term_session_set_prefer_dark (self->session, adw_style_manager_get_dark (ADW_STYLE_MANAGER (object)));
    }
}

static void
corral_ghostty_get_property (GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
    CorralGhostty *self = CORRAL_GHOSTTY (object);
    switch (prop_id) {
    case PROP_COMMAND:
        g_value_set_string (value, self->command);
        break;
    case PROP_CWD:
        g_value_set_string (value, self->cwd);
        break;
    case PROP_FONT_FAMILY:
        g_value_set_string (value, self->font_family);
        break;
    case PROP_FONT_SIZE:
        g_value_set_float (value, self->font_size);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
        break;
    }
}

static void
corral_ghostty_set_property (GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec)
{
    CorralGhostty *self = CORRAL_GHOSTTY (object);
    switch (prop_id) {
    case PROP_COMMAND:
        g_free (self->command);
        self->command = g_value_dup_string (value);
        break;
    case PROP_CWD:
        g_free (self->cwd);
        self->cwd = g_value_dup_string (value);
        break;
    case PROP_FONT_FAMILY: {
        const char *next = g_value_get_string (value);
        if (g_strcmp0 (self->font_family, next) == 0) {
            break;
        }
        g_free (self->font_family);
        self->font_family = g_strdup (next);
        apply_font (self);
        break;
    }
    case PROP_FONT_SIZE:
        self->font_size = g_value_get_float (value);
        apply_font (self);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
        break;
    }
}

static void
corral_ghostty_dispose (GObject *object)
{
    CorralGhostty *self = CORRAL_GHOSTTY (object);

    g_clear_handle_id (&self->blink_source, g_source_remove);
    g_clear_handle_id (&self->hold_source, g_source_remove);
    g_signal_handlers_disconnect_by_data (adw_style_manager_get_default (), self);
    if (self->session != NULL) {
        g_signal_handlers_disconnect_by_data (self->session, self);
        corral_term_session_close (self->session);
        g_clear_object (&self->session);
    }
    g_clear_pointer (&self->placements, g_array_unref);
    G_OBJECT_CLASS (corral_ghostty_parent_class)->dispose (object);
}

static void
corral_ghostty_finalize (GObject *object)
{
    CorralGhostty *self = CORRAL_GHOSTTY (object);

    g_ptr_array_unref (self->row_bg);
    g_ptr_array_unref (self->row_fg);
    ghostty_render_state_row_cells_free (self->cells);
    ghostty_render_state_row_iterator_free (self->rows);
    ghostty_render_state_image_iterator_free (self->images);
    ghostty_render_state_free (self->render);
    g_clear_pointer (&self->cursor.text, g_free);
    g_clear_object (&self->pango);
    g_clear_pointer (&self->font, pango_font_description_free);
    corral_appearance_clear (&self->appearance);
    g_free (self->command);
    g_free (self->cwd);
    g_free (self->font_family);
    G_OBJECT_CLASS (corral_ghostty_parent_class)->finalize (object);
}

static void
corral_ghostty_class_init (CorralGhosttyClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

    object_class->get_property = corral_ghostty_get_property;
    object_class->set_property = corral_ghostty_set_property;
    object_class->dispose = corral_ghostty_dispose;
    object_class->finalize = corral_ghostty_finalize;
    widget_class->snapshot = corral_ghostty_snapshot;
    widget_class->size_allocate = corral_ghostty_size_allocate;

    properties[PROP_COMMAND] = g_param_spec_string (
        "command", NULL, NULL, NULL,
        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    properties[PROP_CWD] = g_param_spec_string (
        "cwd", NULL, NULL, NULL,
        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    properties[PROP_FONT_FAMILY] = g_param_spec_string (
        "font-family", NULL, NULL, NULL,
        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    properties[PROP_FONT_SIZE] = g_param_spec_float (
        "font-size", NULL, NULL, 8.0, 48.0, 10.0,
        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    g_object_class_install_properties (object_class, N_PROPS, properties);

    signals[SIGNAL_EXITED] = g_signal_new (
        "exited",
        G_TYPE_FROM_CLASS (klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 1, G_TYPE_INT);
    signals[SIGNAL_TITLE_CHANGED] = g_signal_new (
        "title-changed",
        G_TYPE_FROM_CLASS (klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 1, G_TYPE_STRING);

    /* Set before any terminal exists, while only this thread uses the library. */
    ghostty_sys_set (GHOSTTY_SYS_OPT_DECODE_PNG, (const void *) decode_png);
}

static void
corral_ghostty_init (CorralGhostty *self)
{
    GtkWidget *widget = GTK_WIDGET (self);

    self->font_size = 10.0f;
    self->blink_on = TRUE;
    self->mouse_shape = GHOSTTY_MOUSE_SHAPE_TEXT;
    self->row_bg = g_ptr_array_new_with_free_func (free_node);
    self->row_fg = g_ptr_array_new_with_free_func (free_node);
    self->colors = (GhosttyRenderStateColors) GHOSTTY_INIT_SIZED (GhosttyRenderStateColors);
    ghostty_render_state_new (NULL, &self->render);
    ghostty_render_state_row_iterator_new (NULL, &self->rows);
    ghostty_render_state_row_cells_new (NULL, &self->cells);
    ghostty_render_state_image_iterator_new (NULL, &self->images);
    corral_appearance_init (&self->appearance);
    load_appearance (self);
    load_font (self);

    gtk_widget_set_hexpand (widget, TRUE);
    gtk_widget_set_vexpand (widget, TRUE);
    gtk_widget_set_focusable (widget, TRUE);
    gtk_widget_set_focus_on_click (widget, TRUE);
    gtk_widget_set_overflow (widget, GTK_OVERFLOW_HIDDEN);
    gtk_widget_set_cursor_from_name (widget, "text");

    g_signal_connect (self, "notify::scale-factor", G_CALLBACK (on_scale_factor), NULL);
    g_signal_connect (adw_style_manager_get_default (), "notify::dark", G_CALLBACK (on_dark_changed), self);

    GtkEventController *keys = gtk_event_controller_key_new ();
    gtk_event_controller_set_propagation_phase (keys, GTK_PHASE_CAPTURE);
    g_signal_connect (keys, "key-pressed", G_CALLBACK (on_key_pressed), self);
    g_signal_connect (keys, "key-released", G_CALLBACK (on_key_released), self);
    gtk_widget_add_controller (widget, keys);

    GtkEventController *pointer = gtk_event_controller_legacy_new ();
    g_signal_connect (pointer, "event", G_CALLBACK (on_pointer_event), self);
    gtk_widget_add_controller (widget, pointer);

    GtkEventController *scroll = gtk_event_controller_scroll_new (GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
    g_signal_connect (scroll, "scroll", G_CALLBACK (on_scroll), self);
    g_signal_connect (scroll, "scroll-begin", G_CALLBACK (on_scroll_begin), self);
    g_signal_connect (scroll, "scroll-end", G_CALLBACK (on_scroll_end), self);
    gtk_widget_add_controller (widget, scroll);

    GtkEventController *focus = gtk_event_controller_focus_new ();
    g_signal_connect (focus, "enter", G_CALLBACK (on_focus_enter), self);
    g_signal_connect (focus, "leave", G_CALLBACK (on_focus_leave), self);
    gtk_widget_add_controller (widget, focus);
}

CorralGhostty *
corral_ghostty_new (void)
{
    return g_object_new (CORRAL_TYPE_GHOSTTY, NULL);
}

const char *
corral_ghostty_get_command (CorralGhostty *self)
{
    g_return_val_if_fail (CORRAL_IS_GHOSTTY (self), NULL);
    return self->command;
}

void
corral_ghostty_set_command (CorralGhostty *self, const char *command)
{
    g_return_if_fail (CORRAL_IS_GHOSTTY (self));
    g_object_set (self, "command", command, NULL);
}

const char *
corral_ghostty_get_cwd (CorralGhostty *self)
{
    g_return_val_if_fail (CORRAL_IS_GHOSTTY (self), NULL);
    return self->cwd;
}

void
corral_ghostty_set_cwd (CorralGhostty *self, const char *cwd)
{
    g_return_if_fail (CORRAL_IS_GHOSTTY (self));
    g_object_set (self, "cwd", cwd, NULL);
}

const char *
corral_ghostty_get_font_family (CorralGhostty *self)
{
    g_return_val_if_fail (CORRAL_IS_GHOSTTY (self), NULL);
    return self->font_family;
}

void
corral_ghostty_set_font_family (CorralGhostty *self, const char *font_family)
{
    g_return_if_fail (CORRAL_IS_GHOSTTY (self));
    g_object_set (self, "font-family", font_family, NULL);
}

float
corral_ghostty_get_font_size (CorralGhostty *self)
{
    g_return_val_if_fail (CORRAL_IS_GHOSTTY (self), 13.0f);
    return self->font_size;
}

void
corral_ghostty_set_font_size (CorralGhostty *self, float font_size)
{
    g_return_if_fail (CORRAL_IS_GHOSTTY (self));
    g_object_set (self, "font-size", font_size, NULL);
}

gboolean
corral_ghostty_start (CorralGhostty *self, GError **error)
{
    g_return_val_if_fail (CORRAL_IS_GHOSTTY (self), FALSE);
    self->started = TRUE;
    ensure_session (self);
    return TRUE;
}
