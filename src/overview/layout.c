/* Lanes and slots on screen. */
#include "overview/state.h"

#define MARGIN    44
#define LANE_GAP  18
#define BADGE_GAP 18
#define ZOOM_MAX  0.5
#define ZOOM_MIN  0.02

typedef struct {
	int lane_x, lane_w, lane_h, top;
	int inner_x, inner_w;
	int mon_h;
	double scale;
} Frame;

struct nx_box
box_lerp(struct nx_box a, struct nx_box b, double t)
{
	return (struct nx_box){
		(int)lround(a.x + (b.x - a.x) * t),
		(int)lround(a.y + (b.y - a.y) * t),
		(int)lround(a.width + (b.width - a.width) * t),
		(int)lround(a.height + (b.height - a.height) * t),
	};
}

/* Content box with ref active. */
struct nx_box
grid_screen_box(Client *c, Workspace *ref)
{
	Workspace *ws = c->column->ws;
	Monitor *m = ws->mon;
	struct nx_box b = tile_rest_box(c);
	int bw = (int)c->bw;

	b.x += m->w.x - ws->target_scroll_x + bw;
	b.y += m->w.y + (ws->idx - ref->idx) * m->m.height + bw;
	b.width -= 2 * bw;
	b.height -= 2 * bw;
	return b;
}

static Frame
frame_for(Monitor *m, int n_rows)
{
	int avail = m->m.height - 2 * MARGIN - (n_rows - 1) * LANE_GAP;
	Frame f;

	f.mon_h = MAX(monitor_rest_area(m).height, 1);
	f.scale = (double)(avail / n_rows - 2 * OV_LANE_PAD) / f.mon_h;
	f.scale = MAX(MIN(f.scale, ZOOM_MAX), ZOOM_MIN);
	f.lane_h = (int)lround(f.scale * f.mon_h) + 2 * OV_LANE_PAD;
	f.top = m->m.y + (m->m.height - n_rows * f.lane_h
			- (n_rows - 1) * LANE_GAP) / 2;
	f.lane_x = m->m.x + MARGIN;
	f.lane_w = m->m.width - 2 * MARGIN;
	f.inner_x = f.lane_x + OV_LANE_PAD + OV_BADGE + BADGE_GAP;
	f.inner_w = f.lane_x + f.lane_w - OV_LANE_PAD - f.inner_x;
	return f;
}

/* Stack cells by real share. */
static int
layout_column(int first, int k, struct nx_box box)
{
	double sum = 0.0, y = box.y;
	int avail = box.height - (k - 1) * OV_TILE_GAP;

	for (int i = first; i < first + k; i++) {
		ov.cells[i].slot.height = MAX(tile_rest_box(ov.cells[i].c).height, 1);
		sum += ov.cells[i].slot.height;
	}
	for (int i = first; i < first + k; i++) {
		double h = avail * ov.cells[i].slot.height / sum;

		ov.cells[i].slot = (struct nx_box){ box.x, (int)lround(y),
				box.width, (int)lround(y + h) - (int)lround(y) };
		y += h + OV_TILE_GAP;
	}
	return first + k;
}

static struct nx_box
lane_box(const Frame *f, int r)
{
	return (struct nx_box){ f->lane_x,
			f->top + r * (f->lane_h + LANE_GAP), f->lane_w, f->lane_h };
}

static int
layout_row(const Frame *f, int r, int first)
{
	Row *row = &ov.rows[r];
	Column *col;
	double total = 0.0, scale, x;
	int cols = 0, h, y;

	row->box = lane_box(f, r);
	wl_list_for_each(col, &row->ws->columns, link) {
		if (!column_shown(col))
			continue;
		total += col->target_width;
		cols++;
	}
	scale = MIN(f->scale, (f->inner_w - (cols - 1) * OV_TILE_GAP) / MAX(total, 1.0));
	scale = MAX(scale, ZOOM_MIN);
	x = f->inner_x + (f->inner_w - total * scale - (cols - 1) * OV_TILE_GAP) / 2.0;
	h = (int)lround(f->mon_h * scale);
	y = row->box.y + (row->box.height - h) / 2;
	wl_list_for_each(col, &row->ws->columns, link) {
		int k = column_shown(col);

		if (!k)
			continue;
		first = layout_column(first, k, (struct nx_box){ (int)lround(x), y,
				(int)lround(col->target_width * scale), h });
		x += col->target_width * scale + OV_TILE_GAP;
	}
	return first;
}

/* Slots, plus the ghost lane. */
void
grid_layout(Monitor *m)
{
	Frame f = frame_for(m, ov.n_rows + ov.ghost.shown);
	int first = 0;

	for (int r = 0; r < ov.n_rows; r++)
		first = layout_row(&f, r, first);
	if (ov.ghost.shown)
		ov.ghost.box = lane_box(&f, ov.n_rows);
}

/* Current boxes become start points. */
void
grid_freeze(void)
{
	for (int i = 0; i < ov.n_cells; i++)
		ov.cells[i].from = box_lerp(ov.cells[i].from, ov.cells[i].slot,
				ov.flow.pos);
	for (int r = 0; r < ov.n_rows; r++)
		ov.rows[r].from = box_lerp(ov.rows[r].from, ov.rows[r].box,
				ov.flow.pos);
	ov.ghost.from = box_lerp(ov.ghost.from, ov.ghost.box, ov.flow.pos);
}
