/* A port of Ghostty 1.4's sprite font (src/font/sprite/draw/), which draws
 * these glyphs from the cell metrics so they join across cells. */

#include "term-sprites.h"

#include <math.h>

enum { NONE, LIGHT, HEAVY, DOUBLE };

#define LINES(up, right, down, left) ((up) | (right) << 2 | (down) << 4 | (left) << 6)

static const guint8 box_lines[0x80] = {
    [0x00] = LINES (NONE, LIGHT, NONE, LIGHT),
    [0x01] = LINES (NONE, HEAVY, NONE, HEAVY),
    [0x02] = LINES (LIGHT, NONE, LIGHT, NONE),
    [0x03] = LINES (HEAVY, NONE, HEAVY, NONE),
    [0x04 ... 0x0b] = 0,
    [0x0c] = LINES (NONE, LIGHT, LIGHT, NONE),
    [0x0d] = LINES (NONE, HEAVY, LIGHT, NONE),
    [0x0e] = LINES (NONE, LIGHT, HEAVY, NONE),
    [0x0f] = LINES (NONE, HEAVY, HEAVY, NONE),
    [0x10] = LINES (NONE, NONE, LIGHT, LIGHT),
    [0x11] = LINES (NONE, NONE, LIGHT, HEAVY),
    [0x12] = LINES (NONE, NONE, HEAVY, LIGHT),
    [0x13] = LINES (NONE, NONE, HEAVY, HEAVY),
    [0x14] = LINES (LIGHT, LIGHT, NONE, NONE),
    [0x15] = LINES (LIGHT, HEAVY, NONE, NONE),
    [0x16] = LINES (HEAVY, LIGHT, NONE, NONE),
    [0x17] = LINES (HEAVY, HEAVY, NONE, NONE),
    [0x18] = LINES (LIGHT, NONE, NONE, LIGHT),
    [0x19] = LINES (LIGHT, NONE, NONE, HEAVY),
    [0x1a] = LINES (HEAVY, NONE, NONE, LIGHT),
    [0x1b] = LINES (HEAVY, NONE, NONE, HEAVY),
    [0x1c] = LINES (LIGHT, LIGHT, LIGHT, NONE),
    [0x1d] = LINES (LIGHT, HEAVY, LIGHT, NONE),
    [0x1e] = LINES (HEAVY, LIGHT, LIGHT, NONE),
    [0x1f] = LINES (LIGHT, LIGHT, HEAVY, NONE),
    [0x20] = LINES (HEAVY, LIGHT, HEAVY, NONE),
    [0x21] = LINES (HEAVY, HEAVY, LIGHT, NONE),
    [0x22] = LINES (LIGHT, HEAVY, HEAVY, NONE),
    [0x23] = LINES (HEAVY, HEAVY, HEAVY, NONE),
    [0x24] = LINES (LIGHT, NONE, LIGHT, LIGHT),
    [0x25] = LINES (LIGHT, NONE, LIGHT, HEAVY),
    [0x26] = LINES (HEAVY, NONE, LIGHT, LIGHT),
    [0x27] = LINES (LIGHT, NONE, HEAVY, LIGHT),
    [0x28] = LINES (HEAVY, NONE, HEAVY, LIGHT),
    [0x29] = LINES (HEAVY, NONE, LIGHT, HEAVY),
    [0x2a] = LINES (LIGHT, NONE, HEAVY, HEAVY),
    [0x2b] = LINES (HEAVY, NONE, HEAVY, HEAVY),
    [0x2c] = LINES (NONE, LIGHT, LIGHT, LIGHT),
    [0x2d] = LINES (NONE, LIGHT, LIGHT, HEAVY),
    [0x2e] = LINES (NONE, HEAVY, LIGHT, LIGHT),
    [0x2f] = LINES (NONE, HEAVY, LIGHT, HEAVY),
    [0x30] = LINES (NONE, LIGHT, HEAVY, LIGHT),
    [0x31] = LINES (NONE, LIGHT, HEAVY, HEAVY),
    [0x32] = LINES (NONE, HEAVY, HEAVY, LIGHT),
    [0x33] = LINES (NONE, HEAVY, HEAVY, HEAVY),
    [0x34] = LINES (LIGHT, LIGHT, NONE, LIGHT),
    [0x35] = LINES (LIGHT, LIGHT, NONE, HEAVY),
    [0x36] = LINES (LIGHT, HEAVY, NONE, LIGHT),
    [0x37] = LINES (LIGHT, HEAVY, NONE, HEAVY),
    [0x38] = LINES (HEAVY, LIGHT, NONE, LIGHT),
    [0x39] = LINES (HEAVY, LIGHT, NONE, HEAVY),
    [0x3a] = LINES (HEAVY, HEAVY, NONE, LIGHT),
    [0x3b] = LINES (HEAVY, HEAVY, NONE, HEAVY),
    [0x3c] = LINES (LIGHT, LIGHT, LIGHT, LIGHT),
    [0x3d] = LINES (LIGHT, LIGHT, LIGHT, HEAVY),
    [0x3e] = LINES (LIGHT, HEAVY, LIGHT, LIGHT),
    [0x3f] = LINES (LIGHT, HEAVY, LIGHT, HEAVY),
    [0x40] = LINES (HEAVY, LIGHT, LIGHT, LIGHT),
    [0x41] = LINES (LIGHT, LIGHT, HEAVY, LIGHT),
    [0x42] = LINES (HEAVY, LIGHT, HEAVY, LIGHT),
    [0x43] = LINES (HEAVY, LIGHT, LIGHT, HEAVY),
    [0x44] = LINES (HEAVY, HEAVY, LIGHT, LIGHT),
    [0x45] = LINES (LIGHT, LIGHT, HEAVY, HEAVY),
    [0x46] = LINES (LIGHT, HEAVY, HEAVY, LIGHT),
    [0x47] = LINES (HEAVY, HEAVY, LIGHT, HEAVY),
    [0x48] = LINES (LIGHT, HEAVY, HEAVY, HEAVY),
    [0x49] = LINES (HEAVY, LIGHT, HEAVY, HEAVY),
    [0x4a] = LINES (HEAVY, HEAVY, HEAVY, LIGHT),
    [0x4b] = LINES (HEAVY, HEAVY, HEAVY, HEAVY),
    [0x4c ... 0x4f] = 0,
    [0x50] = LINES (NONE, DOUBLE, NONE, DOUBLE),
    [0x51] = LINES (DOUBLE, NONE, DOUBLE, NONE),
    [0x52] = LINES (NONE, DOUBLE, LIGHT, NONE),
    [0x53] = LINES (NONE, LIGHT, DOUBLE, NONE),
    [0x54] = LINES (NONE, DOUBLE, DOUBLE, NONE),
    [0x55] = LINES (NONE, NONE, LIGHT, DOUBLE),
    [0x56] = LINES (NONE, NONE, DOUBLE, LIGHT),
    [0x57] = LINES (NONE, NONE, DOUBLE, DOUBLE),
    [0x58] = LINES (LIGHT, DOUBLE, NONE, NONE),
    [0x59] = LINES (DOUBLE, LIGHT, NONE, NONE),
    [0x5a] = LINES (DOUBLE, DOUBLE, NONE, NONE),
    [0x5b] = LINES (LIGHT, NONE, NONE, DOUBLE),
    [0x5c] = LINES (DOUBLE, NONE, NONE, LIGHT),
    [0x5d] = LINES (DOUBLE, NONE, NONE, DOUBLE),
    [0x5e] = LINES (LIGHT, DOUBLE, LIGHT, NONE),
    [0x5f] = LINES (DOUBLE, LIGHT, DOUBLE, NONE),
    [0x60] = LINES (DOUBLE, DOUBLE, DOUBLE, NONE),
    [0x61] = LINES (LIGHT, NONE, LIGHT, DOUBLE),
    [0x62] = LINES (DOUBLE, NONE, DOUBLE, LIGHT),
    [0x63] = LINES (DOUBLE, NONE, DOUBLE, DOUBLE),
    [0x64] = LINES (NONE, DOUBLE, LIGHT, DOUBLE),
    [0x65] = LINES (NONE, LIGHT, DOUBLE, LIGHT),
    [0x66] = LINES (NONE, DOUBLE, DOUBLE, DOUBLE),
    [0x67] = LINES (LIGHT, DOUBLE, NONE, DOUBLE),
    [0x68] = LINES (DOUBLE, LIGHT, NONE, LIGHT),
    [0x69] = LINES (DOUBLE, DOUBLE, NONE, DOUBLE),
    [0x6a] = LINES (LIGHT, DOUBLE, LIGHT, DOUBLE),
    [0x6b] = LINES (DOUBLE, LIGHT, DOUBLE, LIGHT),
    [0x6c] = LINES (DOUBLE, DOUBLE, DOUBLE, DOUBLE),
    [0x6d ... 0x73] = 0,
    [0x74] = LINES (NONE, NONE, NONE, LIGHT),
    [0x75] = LINES (LIGHT, NONE, NONE, NONE),
    [0x76] = LINES (NONE, LIGHT, NONE, NONE),
    [0x77] = LINES (NONE, NONE, LIGHT, NONE),
    [0x78] = LINES (NONE, NONE, NONE, HEAVY),
    [0x79] = LINES (HEAVY, NONE, NONE, NONE),
    [0x7a] = LINES (NONE, HEAVY, NONE, NONE),
    [0x7b] = LINES (NONE, NONE, HEAVY, NONE),
    [0x7c] = LINES (NONE, HEAVY, NONE, LIGHT),
    [0x7d] = LINES (LIGHT, NONE, HEAVY, NONE),
    [0x7e] = LINES (NONE, LIGHT, NONE, HEAVY),
    [0x7f] = LINES (HEAVY, NONE, LIGHT, NONE),
};

typedef struct {
    GtkSnapshot *snapshot;
    const CorralSpriteMetrics *m;
    GdkRGBA color;
} Canvas;

static int
sub (int a, int b)
{
    return a > b ? a - b : 0;
}

static void
box_alpha (Canvas *c, int x0, int y0, int x1, int y1, double alpha)
{
    x0 = MAX (x0, 0);
    y0 = MAX (y0, 0);
    x1 = MIN (x1, c->m->cell_width);
    y1 = MIN (y1, c->m->cell_height);
    if (x1 <= x0 || y1 <= y0)
        return;
    GdkRGBA color = c->color;
    color.alpha *= alpha;
    gtk_snapshot_append_color (c->snapshot, &color, &GRAPHENE_RECT_INIT (x0, y0, x1 - x0, y1 - y0));
}

static void
box (Canvas *c, int x0, int y0, int x1, int y1)
{
    box_alpha (c, x0, y0, x1, y1, 1.0);
}

static void
stroke (Canvas *c, GskPathBuilder *builder, double width)
{
    GskPath *path = gsk_path_builder_free_to_path (builder);
    GskStroke *s = gsk_stroke_new (width);
    gsk_stroke_set_line_cap (s, GSK_LINE_CAP_BUTT);
    gtk_snapshot_push_clip (c->snapshot, &GRAPHENE_RECT_INIT (0, 0, c->m->cell_width, c->m->cell_height));
    gtk_snapshot_append_stroke (c->snapshot, path, s, &c->color);
    gtk_snapshot_pop (c->snapshot);
    gsk_stroke_free (s);
    gsk_path_unref (path);
}

static void
fill (Canvas *c, GskPathBuilder *builder)
{
    GskPath *path = gsk_path_builder_free_to_path (builder);
    gtk_snapshot_append_fill (c->snapshot, path, GSK_FILL_RULE_WINDING, &c->color);
    gsk_path_unref (path);
}

static void
lines_char (Canvas *c, guint8 lines)
{
    int up = lines & 3, right = lines >> 2 & 3, down = lines >> 4 & 3, left = lines >> 6 & 3;
    int w = c->m->cell_width, h = c->m->cell_height;
    int light = c->m->box_thickness, heavy = light * 2;

    int h_light_top = sub (h, light) / 2;
    int h_light_bottom = h_light_top + light;
    int h_heavy_top = sub (h, heavy) / 2;
    int h_heavy_bottom = h_heavy_top + heavy;
    int h_double_top = sub (h_light_top, light);
    int h_double_bottom = h_light_bottom + light;
    int v_light_left = sub (w, light) / 2;
    int v_light_right = v_light_left + light;
    int v_heavy_left = sub (w, heavy) / 2;
    int v_heavy_right = v_heavy_left + heavy;
    int v_double_left = sub (v_light_left, light);
    int v_double_right = v_light_right + light;

    int up_bottom = (left == HEAVY || right == HEAVY) ? h_heavy_bottom
                  : (left != right || down == up)    ? ((left == DOUBLE || right == DOUBLE) ? h_double_bottom : h_light_bottom)
                  : (left == NONE && right == NONE)  ? h_light_bottom
                                                     : h_light_top;
    int down_top = (left == HEAVY || right == HEAVY) ? h_heavy_top
                 : (left != right || up == down)     ? ((left == DOUBLE || right == DOUBLE) ? h_double_top : h_light_top)
                 : (left == NONE && right == NONE)   ? h_light_top
                                                     : h_light_bottom;
    int left_right = (up == HEAVY || down == HEAVY) ? v_heavy_right
                   : (up != down || left == right)  ? ((up == DOUBLE || down == DOUBLE) ? v_double_right : v_light_right)
                   : (up == NONE && down == NONE)   ? v_light_right
                                                    : v_light_left;
    int right_left = (up == HEAVY || down == HEAVY) ? v_heavy_left
                   : (up != down || right == left)  ? ((up == DOUBLE || down == DOUBLE) ? v_double_left : v_light_left)
                   : (up == NONE && down == NONE)   ? v_light_left
                                                    : v_light_right;

    switch (up) {
    case LIGHT: box (c, v_light_left, 0, v_light_right, up_bottom); break;
    case HEAVY: box (c, v_heavy_left, 0, v_heavy_right, up_bottom); break;
    case DOUBLE:
        box (c, v_double_left, 0, v_light_left, left == DOUBLE ? h_light_top : up_bottom);
        box (c, v_light_right, 0, v_double_right, right == DOUBLE ? h_light_top : up_bottom);
        break;
    }
    switch (right) {
    case LIGHT: box (c, right_left, h_light_top, w, h_light_bottom); break;
    case HEAVY: box (c, right_left, h_heavy_top, w, h_heavy_bottom); break;
    case DOUBLE:
        box (c, up == DOUBLE ? v_light_right : right_left, h_double_top, w, h_light_top);
        box (c, down == DOUBLE ? v_light_right : right_left, h_light_bottom, w, h_double_bottom);
        break;
    }
    switch (down) {
    case LIGHT: box (c, v_light_left, down_top, v_light_right, h); break;
    case HEAVY: box (c, v_heavy_left, down_top, v_heavy_right, h); break;
    case DOUBLE:
        box (c, v_double_left, left == DOUBLE ? h_light_bottom : down_top, v_light_left, h);
        box (c, v_light_right, right == DOUBLE ? h_light_bottom : down_top, v_double_right, h);
        break;
    }
    switch (left) {
    case LIGHT: box (c, 0, h_light_top, left_right, h_light_bottom); break;
    case HEAVY: box (c, 0, h_heavy_top, left_right, h_heavy_bottom); break;
    case DOUBLE:
        box (c, 0, h_double_top, up == DOUBLE ? v_light_left : left_right, h_light_top);
        box (c, 0, h_light_bottom, down == DOUBLE ? v_light_left : left_right, h_double_bottom);
        break;
    }
}

static void
dash_horizontal (Canvas *c, int count, int thick, int desired_gap)
{
    int w = c->m->cell_width, h = c->m->cell_height;

    if (w < count * 2) {
        lines_char (c, LINES (NONE, LIGHT, NONE, LIGHT));
        return;
    }
    int gap = MIN (desired_gap, w / (2 * count));
    int dashes = w - count * gap;
    int dash = dashes / count;
    int extra = dashes % count;
    int y = sub (h, thick) / 2;
    int x = gap / 2;
    for (int i = 0; i < count; i++) {
        int x1 = x + dash;
        if (extra > 0) {
            extra--;
            x1++;
        }
        box (c, x, y, x1, y + thick);
        x = x1 + gap;
    }
}

static void
dash_vertical (Canvas *c, int count, int thick, int desired_gap)
{
    int w = c->m->cell_width, h = c->m->cell_height;

    if (h < count * 2) {
        lines_char (c, LINES (LIGHT, NONE, LIGHT, NONE));
        return;
    }
    int gap = MIN (desired_gap, h / (2 * count));
    int dashes = h - count * gap;
    int dash = dashes / count;
    int extra = dashes % count;
    int x = sub (w, thick) / 2;
    int y = 0;
    for (int i = 0; i < count; i++) {
        int y1 = y + dash;
        if (extra > 0) {
            extra--;
            y1++;
        }
        box (c, x, y, x + thick, y1);
        y = y1 + gap;
    }
}

static void
dashes (Canvas *c, guint32 cp)
{
    int light = c->m->box_thickness, heavy = light * 2;
    int gap4 = MAX (4, light);

    switch (cp) {
    case 0x2504: dash_horizontal (c, 3, light, gap4); break;
    case 0x2505: dash_horizontal (c, 3, heavy, gap4); break;
    case 0x2506: dash_vertical (c, 3, light, gap4); break;
    case 0x2507: dash_vertical (c, 3, heavy, gap4); break;
    case 0x2508: dash_horizontal (c, 4, light, gap4); break;
    case 0x2509: dash_horizontal (c, 4, heavy, gap4); break;
    case 0x250a: dash_vertical (c, 4, light, gap4); break;
    case 0x250b: dash_vertical (c, 4, heavy, gap4); break;
    case 0x254c: dash_horizontal (c, 2, light, light); break;
    case 0x254d: dash_horizontal (c, 2, heavy, heavy); break;
    case 0x254e: dash_vertical (c, 2, light, heavy); break;
    case 0x254f: dash_vertical (c, 2, heavy, heavy); break;
    }
}

static void
diagonal (Canvas *c, gboolean down_right)
{
    double w = c->m->cell_width, h = c->m->cell_height;
    double sx = MIN (1.0, w / h), sy = MIN (1.0, h / w);
    GskPathBuilder *b = gsk_path_builder_new ();

    if (down_right) {
        gsk_path_builder_move_to (b, -0.5 * sx, -0.5 * sy);
        gsk_path_builder_line_to (b, w + 0.5 * sx, h + 0.5 * sy);
    } else {
        gsk_path_builder_move_to (b, w + 0.5 * sx, -0.5 * sy);
        gsk_path_builder_line_to (b, -0.5 * sx, h + 0.5 * sy);
    }
    stroke (c, b, c->m->box_thickness);
}

/* CORNER names the cell corner the arc bends toward: 0 tl, 1 tr, 2 bl, 3 br. */
static void
arc (Canvas *c, int corner)
{
    int thick = c->m->box_thickness;
    double w = c->m->cell_width, h = c->m->cell_height;
    double cx = sub (c->m->cell_width, thick) / 2 + thick / 2.0;
    double cy = sub (c->m->cell_height, thick) / 2 + thick / 2.0;
    double r = MIN (w, h) / 2, s = 0.25;
    double vy = corner < 2 ? -1 : 1, hx = (corner & 1) ? 1 : -1;
    GskPathBuilder *b = gsk_path_builder_new ();

    gsk_path_builder_move_to (b, cx, corner < 2 ? 0 : h);
    gsk_path_builder_line_to (b, cx, cy + vy * r);
    gsk_path_builder_cubic_to (b, cx, cy + vy * s * r, cx + hx * s * r, cy, cx + hx * r, cy);
    gsk_path_builder_line_to (b, (corner & 1) ? w : 0, cy);
    stroke (c, b, thick);
}

static int
fraction_min (double f, int size)
{
    return size - (int) round ((1.0 - f) * size);
}

static int
fraction_max (double f, int size)
{
    return (int) round (f * size);
}

static void
quadrants (Canvas *c, gboolean tl, gboolean tr, gboolean bl, gboolean br)
{
    int w = c->m->cell_width, h = c->m->cell_height;
    int x_mid0 = fraction_min (0.5, w), x_mid1 = fraction_max (0.5, w);
    int y_mid0 = fraction_min (0.5, h), y_mid1 = fraction_max (0.5, h);

    if (tl)
        box (c, 0, 0, x_mid1, y_mid1);
    if (tr)
        box (c, x_mid0, 0, w, y_mid1);
    if (bl)
        box (c, 0, y_mid0, x_mid1, h);
    if (br)
        box (c, x_mid0, y_mid0, w, h);
}

typedef enum { ALIGN_UPPER, ALIGN_LOWER, ALIGN_LEFT, ALIGN_RIGHT } Align;

static void
block (Canvas *c, Align align, double fw, double fh)
{
    int cw = c->m->cell_width, ch = c->m->cell_height;
    int w = (int) round (cw * fw), h = (int) round (ch * fh);
    int x = align == ALIGN_LEFT ? 0 : align == ALIGN_RIGHT ? cw - w : (cw - w) / 2;
    int y = align == ALIGN_UPPER ? 0 : align == ALIGN_LOWER ? ch - h : (ch - h) / 2;

    box (c, x, y, x + w, y + h);
}

static void
blocks (Canvas *c, guint32 cp)
{
    int w = c->m->cell_width, h = c->m->cell_height;

    switch (cp) {
    case 0x2580: block (c, ALIGN_UPPER, 1, 0.5); break;
    case 0x2581 ... 0x2587: block (c, ALIGN_LOWER, 1, (cp - 0x2580) / 8.0); break;
    case 0x2588: box (c, 0, 0, w, h); break;
    case 0x2589 ... 0x258f: block (c, ALIGN_LEFT, (0x2590 - cp) / 8.0, 1); break;
    case 0x2590: block (c, ALIGN_RIGHT, 0.5, 1); break;
    case 0x2591: box_alpha (c, 0, 0, w, h, 0x40 / 255.0); break;
    case 0x2592: box_alpha (c, 0, 0, w, h, 0x80 / 255.0); break;
    case 0x2593: box_alpha (c, 0, 0, w, h, 0xc0 / 255.0); break;
    case 0x2594: block (c, ALIGN_UPPER, 1, 0.125); break;
    case 0x2595: block (c, ALIGN_RIGHT, 0.125, 1); break;
    case 0x2596: quadrants (c, FALSE, FALSE, TRUE, FALSE); break;
    case 0x2597: quadrants (c, FALSE, FALSE, FALSE, TRUE); break;
    case 0x2598: quadrants (c, TRUE, FALSE, FALSE, FALSE); break;
    case 0x2599: quadrants (c, TRUE, FALSE, TRUE, TRUE); break;
    case 0x259a: quadrants (c, TRUE, FALSE, FALSE, TRUE); break;
    case 0x259b: quadrants (c, TRUE, TRUE, TRUE, FALSE); break;
    case 0x259c: quadrants (c, TRUE, TRUE, FALSE, TRUE); break;
    case 0x259d: quadrants (c, FALSE, TRUE, FALSE, FALSE); break;
    case 0x259e: quadrants (c, FALSE, TRUE, TRUE, FALSE); break;
    case 0x259f: quadrants (c, FALSE, TRUE, TRUE, TRUE); break;
    }
}

static void
braille (Canvas *c, guint32 cp)
{
    int width = c->m->cell_width, height = c->m->cell_height;
    int w = MIN (width / 4, height / 8);
    int x_spacing = width / 4, y_spacing = height / 8;
    int x_margin = x_spacing / 2, y_margin = y_spacing / 2;
    int x_left = width - 2 * x_margin - x_spacing - 2 * w;
    int y_left = height - 2 * y_margin - 3 * y_spacing - 4 * w;

    if (x_left >= 2 && y_left >= 4 && w == 0) {
        w++;
        x_left -= 2;
        y_left -= 4;
    }
    if (x_left >= 2 && x_margin == 0) {
        x_margin = 1;
        x_left -= 2;
    }
    if (y_left >= 2 && y_margin == 0) {
        y_margin = 1;
        y_left -= 2;
    }
    if (x_left >= 1) {
        x_spacing++;
        x_left--;
    }
    if (y_left >= 3) {
        y_spacing++;
        y_left -= 3;
    }
    if (x_left >= 2) {
        x_margin++;
        x_left -= 2;
    }
    if (y_left >= 2) {
        y_margin++;
        y_left -= 2;
    }
    if (x_left >= 2 && y_left >= 4)
        w++;

    int x[2] = { x_margin, x_margin + w + x_spacing };
    int y[4] = { y_margin };
    for (int i = 1; i < 4; i++)
        y[i] = y[i - 1] + w + y_spacing;

    /* Bit order of the codepoint: tl ul ll tr ur lr bl br. */
    static const int dot_x[8] = { 0, 0, 0, 1, 1, 1, 0, 1 };
    static const int dot_y[8] = { 0, 1, 2, 0, 1, 2, 3, 3 };
    for (int bit = 0; bit < 8; bit++) {
        if (cp & (1u << bit))
            box (c, x[dot_x[bit]], y[dot_y[bit]], x[dot_x[bit]] + w, y[dot_y[bit]] + w);
    }
}

static void
triangle (Canvas *c, double x0, double y0, double x1, double y1, double x2, double y2)
{
    GskPathBuilder *b = gsk_path_builder_new ();
    gsk_path_builder_move_to (b, x0, y0);
    gsk_path_builder_line_to (b, x1, y1);
    gsk_path_builder_line_to (b, x2, y2);
    gsk_path_builder_close (b);
    fill (c, b);
}

static void
half_circle (Canvas *c, gboolean outline)
{
    double w = c->m->cell_width, h = c->m->cell_height;
    double k = (G_SQRT2 - 1.0) * 4.0 / 3.0;
    double r = MIN (w, h / 2);
    GskPathBuilder *b = gsk_path_builder_new ();

    gsk_path_builder_move_to (b, 0, 0);
    if (outline)
        gsk_path_builder_line_to (b, 1, 0);
    gsk_path_builder_cubic_to (b, r * k, 0, r, r - r * k, r, r);
    gsk_path_builder_line_to (b, r, h - r);
    gsk_path_builder_cubic_to (b, r, h - r + r * k, r * k, h, outline ? 1 : 0, h);
    if (outline) {
        gsk_path_builder_line_to (b, 0, h);
        stroke (c, b, c->m->box_thickness);
    } else {
        gsk_path_builder_close (b);
        fill (c, b);
    }
}

static void
chevron (Canvas *c)
{
    double w = c->m->cell_width, h = c->m->cell_height;
    GskPathBuilder *b = gsk_path_builder_new ();

    gsk_path_builder_move_to (b, 0, 0);
    gsk_path_builder_line_to (b, w, h / 2);
    gsk_path_builder_line_to (b, 0, h);
    stroke (c, b, c->m->box_thickness);
}

static void
powerline (Canvas *c, guint32 cp)
{
    double w = c->m->cell_width, h = c->m->cell_height;
    gboolean flip = cp == 0xe0b3 || cp == 0xe0b6 || cp == 0xe0b7;

    if (flip) {
        gtk_snapshot_save (c->snapshot);
        gtk_snapshot_translate (c->snapshot, &GRAPHENE_POINT_INIT (w, 0));
        gtk_snapshot_scale (c->snapshot, -1, 1);
    }
    switch (cp) {
    case 0xe0b0: triangle (c, 0, 0, w, h / 2, 0, h); break;
    case 0xe0b1: case 0xe0b3: chevron (c); break;
    case 0xe0b2: triangle (c, w, 0, 0, h / 2, w, h); break;
    case 0xe0b4: case 0xe0b6: half_circle (c, FALSE); break;
    case 0xe0b5: case 0xe0b7: half_circle (c, TRUE); break;
    case 0xe0b8: triangle (c, 0, 0, w, h, 0, h); break;
    case 0xe0b9: case 0xe0bf: diagonal (c, TRUE); break;
    case 0xe0ba: triangle (c, w, 0, w, h, 0, h); break;
    case 0xe0bb: case 0xe0bd: diagonal (c, FALSE); break;
    case 0xe0bc: triangle (c, 0, 0, w, 0, 0, h); break;
    case 0xe0be: triangle (c, 0, 0, w, 0, w, h); break;
    }
    if (flip)
        gtk_snapshot_restore (c->snapshot);
}

gboolean
corral_sprite_covers (guint32 cp)
{
    return (cp >= 0x2500 && cp <= 0x259f) || (cp >= 0x2800 && cp <= 0x28ff) || (cp >= 0xe0b0 && cp <= 0xe0bf);
}

void
corral_sprite_draw (GtkSnapshot *snapshot, guint32 cp, const CorralSpriteMetrics *metrics, const GdkRGBA *color)
{
    Canvas c = { snapshot, metrics, *color };

    if (cp >= 0x2500 && cp <= 0x257f) {
        /* Dashes are not in the line table: every line pattern fits in a
         * byte, and the all-double cross (U+256C) uses the last value. */
        guint8 lines = box_lines[cp - 0x2500];
        if ((cp >= 0x2504 && cp <= 0x250b) || (cp >= 0x254c && cp <= 0x254f))
            dashes (&c, cp);
        else if (cp >= 0x256d && cp <= 0x2570)
            arc (&c, (int[]) { 3, 2, 0, 1 }[cp - 0x256d]);
        else if (cp == 0x2571)
            diagonal (&c, FALSE);
        else if (cp == 0x2572)
            diagonal (&c, TRUE);
        else if (cp == 0x2573) {
            diagonal (&c, FALSE);
            diagonal (&c, TRUE);
        } else
            lines_char (&c, lines);
    } else if (cp >= 0x2580 && cp <= 0x259f) {
        blocks (&c, cp);
    } else if (cp >= 0x2800 && cp <= 0x28ff) {
        braille (&c, cp);
    } else if (cp >= 0xe0b0 && cp <= 0xe0bf) {
        powerline (&c, cp);
    }
}
