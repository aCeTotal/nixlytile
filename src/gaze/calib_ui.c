#include "calib.h"

#include <cairo.h>
#include <math.h>

#define MARKER 120
#define CORE_R 5.0
#define RING_MIN 9.0
#define RING_MAX 42.0
#define GLOW 12.0
#define TEXT_W 960
#define TEXT_H 140
#define TITLE_PX 30.0
#define HINT_PX 17.0
#define TITLE_AT 0.3f

static const char hint[] = "Hold hodet rolig  ·  Super+Shift+G avbryter";
static const double accent[3] = { 0.54, 0.71, 0.97 };
static const float backdrop[4] = { 0.017f, 0.021f, 0.034f, 0.86f };

static cairo_t *
paint_begin(struct PixmanBuffer *pb)
{
	cairo_surface_t *cs = cairo_image_surface_create_for_data(pb->data, CAIRO_FORMAT_ARGB32,
		pb->base.width, pb->base.height, pb->stride);
	cairo_t *cr = cairo_create(cs);

	cairo_surface_destroy(cs);
	cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
	cairo_paint(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
	return cr;
}

static void
paint_end(cairo_t *cr)
{
	cairo_surface_flush(cairo_get_target(cr));
	cairo_destroy(cr);
}

static void
centred_text(cairo_t *cr, const char *s, double y)
{
	cairo_text_extents_t ext;
	cairo_surface_t *target = cairo_get_target(cr);

	cairo_text_extents(cr, s, &ext);
	cairo_move_to(cr, (cairo_image_surface_get_width(target) - ext.width) / 2 - ext.x_bearing, y);
	cairo_show_text(cr, s);
}

static void
draw_title(struct PixmanBuffer *pb, const char *title, float scale)
{
	cairo_t *cr = paint_begin(pb);

	cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
	cairo_set_font_size(cr, TITLE_PX * scale);
	cairo_set_source_rgba(cr, 1, 1, 1, 0.95);
	centred_text(cr, title, TEXT_H * 0.45 * scale);
	cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
	cairo_set_font_size(cr, HINT_PX * scale);
	cairo_set_source_rgba(cr, 0.78, 0.82, 0.9, 0.7);
	centred_text(cr, hint, TEXT_H * 0.75 * scale);
	paint_end(cr);
}

static void
draw_marker(struct PixmanBuffer *pb, float ring, float scale)
{
	cairo_t *cr = paint_begin(pb);
	double c = MARKER * scale / 2, r = (RING_MIN + (RING_MAX - RING_MIN) * ring) * scale;
	cairo_pattern_t *glow = cairo_pattern_create_radial(c, c, 0, c, c, r + GLOW * scale);

	cairo_pattern_add_color_stop_rgba(glow, 0, accent[0], accent[1], accent[2], 0.35);
	cairo_pattern_add_color_stop_rgba(glow, 1, accent[0], accent[1], accent[2], 0);
	cairo_set_source(cr, glow);
	cairo_arc(cr, c, c, r + GLOW * scale, 0, 2 * M_PI);
	cairo_fill(cr);
	cairo_pattern_destroy(glow);
	cairo_set_line_width(cr, 2.5 * scale);
	cairo_set_source_rgba(cr, accent[0], accent[1], accent[2], 0.95);
	cairo_arc(cr, c, c, r, 0, 2 * M_PI);
	cairo_stroke(cr);
	cairo_set_source_rgba(cr, 1, 1, 1, 1);
	cairo_arc(cr, c, c, CORE_R * scale, 0, 2 * M_PI);
	cairo_fill(cr);
	paint_end(cr);
}

static struct nx_scene_buffer *
canvas_node(CalibUi *ui, struct PixmanBuffer *pb, int w, int h)
{
	struct nx_scene_buffer *node = nx_scene_buffer_create(ui->tree, pb ? &pb->base : NULL);

	nx_scene_buffer_set_dest_size(node, w, h);
	return node;
}

void
calib_ui_show(CalibUi *ui, Monitor *m, const char *title)
{
	int px = (int)lroundf(MARKER * m->nx_output->scale);
	struct PixmanBuffer *text;

	ui->scale = m->nx_output->scale;
	ui->tree = nx_scene_tree_create(layers[LyrOverlay]);
	nx_scene_node_set_position(&ui->tree->node, m->m.x, m->m.y);
	nx_scene_node_raise_to_top(&ui->tree->node);
	nx_scene_rect_create(ui->tree, m->m.width, m->m.height, backdrop);
	text = bar_canvas_new((int)lroundf(TEXT_W * ui->scale), (int)lroundf(TEXT_H * ui->scale));
	if (text)
		draw_title(text, title, ui->scale);
	ui->text = canvas_node(ui, text, TEXT_W, TEXT_H);
	nx_scene_node_set_position(&ui->text->node, (m->m.width - TEXT_W) / 2,
		(int)lroundf((float)m->m.height * TITLE_AT) - TEXT_H / 2);
	if (text)
		nx_buffer_drop(&text->base);
	ui->canvas[0] = bar_canvas_new(px, px);
	ui->canvas[1] = bar_canvas_new(px, px);
	ui->dot = canvas_node(ui, NULL, MARKER, MARKER);
	nx_output_schedule_frame(m->nx_output);
}

/* Double-buffered so redraws land. */
void
calib_ui_marker(CalibUi *ui, Monitor *m, const Marker *mk)
{
	struct PixmanBuffer *pb = ui->canvas[ui->flip];

	ui->flip ^= 1;
	if (!pb)
		return;
	draw_marker(pb, mk->ring, ui->scale);
	nx_scene_buffer_set_buffer(ui->dot, &pb->base);
	nx_scene_buffer_set_dest_size(ui->dot, MARKER, MARKER);
	nx_scene_node_set_position(&ui->dot->node, (int)lroundf(mk->x * (float)m->m.width) - MARKER / 2,
		(int)lroundf(mk->y * (float)m->m.height) - MARKER / 2);
	nx_output_schedule_frame(m->nx_output);
}

void
calib_ui_hide(CalibUi *ui)
{
	if (!ui->tree)
		return;
	nx_scene_node_destroy(&ui->tree->node);
	for (int i = 0; i < 2; i++)
		if (ui->canvas[i])
			nx_buffer_drop(&ui->canvas[i]->base);
	*ui = (CalibUi){0};
}
