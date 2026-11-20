/* Backdrop, workspace lanes and selection. */
#include "overview/state.h"

#define TINT_A      0.42f
#define TINT_SOLO_A 0.92f
#define LANE_A      0.55f
#define MARKER_R    2
#define RING_W      3
#define RING_GAP    4
#define GLOW_SOFT   28
#define GLOW_A      0.32f

struct nx_scene_rect *
shaped_rect(struct nx_scene_tree *parent, const struct nx_scene_rect_shape *shape)
{
	struct nx_scene_rect *r = nx_scene_rect_create(parent, 1, 1,
			(float[4]){ 0 });

	nx_scene_rect_set_shape(r, shape);
	return r;
}

static struct nx_box
grow(struct nx_box b, int by)
{
	return (struct nx_box){ b.x - by, b.y - by,
			b.width + 2 * by, b.height + 2 * by };
}

void
place_rect(struct nx_scene_rect *r, struct nx_box b, Paint paint)
{
	float premul[4];

	bar_color_premul(paint.rgba, paint.alpha, premul);
	nx_scene_rect_set_color(r, premul);
	nx_scene_rect_set_size(r, b.width, b.height);
	nx_scene_node_set_position(&r->node, b.x, b.y);
}

void
chrome_build(void)
{
	Monitor *m = ov.mon;

	ov.tree = nx_scene_tree_create(layers[LyrOverlay]);
	ov.backdrop = backdrop_attach(ov.tree, m);
	ov.tint = nx_scene_rect_create(ov.tree, m->m.width, m->m.height,
			(float[4]){ 0 });
	nx_scene_node_set_position(&ov.tint->node, m->m.x, m->m.y);
	ov.chrome = nx_scene_tree_create(ov.tree);
	ov.glow = shaped_rect(ov.tree,
			&(struct nx_scene_rect_shape){ OV_RADIUS, GLOW_SOFT, 0 });
	ov.tiles = nx_scene_tree_create(ov.tree);
	ov.marker = shaped_rect(ov.tree,
			&(struct nx_scene_rect_shape){ MARKER_R, 0, 0 });
	ov.ring = shaped_rect(ov.tree, &(struct nx_scene_rect_shape){
			OV_RADIUS + RING_GAP + RING_W, 0, RING_W });
}

void
caption_set(Caption *cap, const char *s, const float fg[static 4])
{
	struct fcft_font *f = statusfont.font;
	struct PixmanBuffer *buf;

	if (cap->node)
		nx_scene_node_destroy(&cap->node->node);
	cap->node = NULL;
	if (!f || !(buf = bar_canvas_new(MAX(nixly_text_width(f, s), 1), f->height)))
		return;
	nixly_text_draw(buf->image, f, s, 0, f->ascent, fg, 0);
	cap->node = nx_scene_buffer_create(ov.chrome, &buf->base);
	cap->w = buf->base.width;
	cap->h = buf->base.height;
	nx_buffer_drop(&buf->base);
}

/* Centred in b. */
void
caption_place(const Caption *cap, struct nx_box b, float alpha)
{
	if (!cap->node)
		return;
	nx_scene_buffer_set_opacity(cap->node, alpha);
	nx_scene_node_set_position(&cap->node->node, b.x + (b.width - cap->w) / 2,
			b.y + (b.height - cap->h) / 2);
}

struct nx_box
lane_badge(struct nx_box lane)
{
	return (struct nx_box){ lane.x + OV_LANE_PAD,
			lane.y + (lane.height - OV_BADGE) / 2, OV_BADGE, OV_BADGE };
}

static void
paint_digits(Row *row, const float fg[static 4])
{
	char num[12];

	snprintf(num, sizeof(num), "%d", row->ws->idx + 1);
	caption_set(&row->digits, num, fg);
}

/* New lanes; every badge. */
void
chrome_rows(void)
{
	Workspace *active = monitor_active_ws(ov.mon);

	for (int r = 0; r < ov.n_rows; r++) {
		Row *row = &ov.rows[r];

		if (!row->lane) {
			row->lane = shaped_rect(ov.chrome,
					&(struct nx_scene_rect_shape){ OV_LANE_R, 0, 0 });
			row->badge = shaped_rect(ov.chrome,
					&(struct nx_scene_rect_shape){ OV_BADGE / 2, 0, 0 });
		}
		paint_digits(row, row->ws == active ? statusbar_ws_active_fg : card_col_dim);
	}
}

void
chrome_drop_row(Row *row)
{
	nx_scene_node_destroy(&row->lane->node);
	nx_scene_node_destroy(&row->badge->node);
	if (row->digits.node)
		nx_scene_node_destroy(&row->digits.node->node);
}

static void
place_row(Row *row, Workspace *active)
{
	struct nx_box b = box_lerp(row->from, row->box, ov.flow.pos);
	struct nx_box badge = lane_badge(b);
	float a = row->fresh ? (float)ov.flow.pos : 1.0f;
	int target = ov.drag.active && row == &ov.rows[ov.drag.row];

	place_rect(row->lane, b, (Paint){ statusbar_island_bg,
			(target ? OV_HOT_A : LANE_A) * a });
	place_rect(row->badge, badge, (Paint){ row->ws == active
			? statusbar_ws_active_bg : statusbar_island_bg, a });
	caption_place(&row->digits, badge, a);
}

/* Ring glides between cells. */
struct nx_box
selection_box(void)
{
	return box_lerp(ov.ring_from, cell_box(&ov.cells[ov.sel]), ov.focus.pos);
}

void
chrome_apply(void)
{
	Workspace *active = monitor_active_ws(ov.mon);
	struct nx_box sel = selection_box();
	float z = (float)ov.zoom.pos;
	float tint[4];

	if (ov.backdrop)
		nx_scene_buffer_set_opacity(ov.backdrop, z);
	bar_color_premul(card_col_bg, (ov.backdrop ? TINT_A : TINT_SOLO_A) * z, tint);
	nx_scene_rect_set_color(ov.tint, tint);
	nx_scene_tree_set_opacity(ov.chrome, z);
	for (int r = 0; r < ov.n_rows; r++)
		place_row(&ov.rows[r], active);
	ghost_apply();
	place_rect(ov.glow, grow(sel, GLOW_SOFT), (Paint){ focuscolor, GLOW_A * z });
	place_rect(ov.ring, grow(sel, RING_GAP + RING_W), (Paint){ focuscolor, z });
	nx_scene_node_set_enabled(&ov.marker->node, ov.drag.active
			&& ov.drag.row < ov.n_rows);
	if (ov.drag.active && ov.drag.row < ov.n_rows)
		place_rect(ov.marker, drag_marker(), (Paint){ focuscolor, z });
}
