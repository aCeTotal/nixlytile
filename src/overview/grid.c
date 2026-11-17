/* Workspace rows as scaled miniatures. */
#include "overview/state.h"

#define MARGIN    44
#define LANE_GAP  18
#define BADGE_GAP 18
#define TILE_GAP  14
#define ZOOM_MAX  0.5
#define ZOOM_MIN  0.02

typedef struct {
	int lane_x, lane_w, lane_h, top;
	int inner_x, inner_w;
	int mon_h;
	double scale;
} Frame;

typedef struct {
	Cell *cells;
	int n;
} Pool;

static int
shown(Client *c)
{
	struct nx_surface *s = client_surface(c);

	return s && s->mapped;
}

static int
column_shown(Column *col)
{
	Client *c;
	int n = 0;

	wl_list_for_each(c, &col->clients, column_link)
		n += shown(c);
	return n;
}

static int
ws_shown(Workspace *ws)
{
	Column *col;
	int n = 0;

	wl_list_for_each(col, &ws->columns, link)
		n += column_shown(col);
	return n;
}

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

/* Reuse c's cell, else new. */
static Cell
adopt_cell(Pool *old, Client *c)
{
	Cell cell;

	for (int i = 0; i < old->n; i++) {
		if (old->cells[i].c != c)
			continue;
		cell = old->cells[i];
		old->cells[i].c = NULL;
		return cell;
	}
	return (Cell){ .c = c };
}

static Row
adopt_row(Row *old, int n_old, Workspace *ws)
{
	Row row;

	for (int i = 0; i < n_old; i++) {
		if (old[i].ws_id != ws->window_id)
			continue;
		row = old[i];
		old[i].ws_id = 0;
		row.ws = ws;
		row.fresh = 0;
		return row;
	}
	return (Row){ .ws_id = ws->window_id, .ws = ws, .fresh = n_old > 0 };
}

static void
collect_column(Column *col, Pool *old)
{
	Client *c;

	wl_list_for_each(c, &col->clients, column_link)
		if (shown(c))
			ov.cells[ov.n_cells++] = adopt_cell(old, c);
}

/* Drop what left the grid. */
static void
release(Pool *old, Row *old_rows, int n_old_rows)
{
	for (int i = 0; i < old->n; i++)
		if (old->cells[i].c)
			cell_destroy(&old->cells[i]);
	for (int i = 0; i < n_old_rows; i++)
		if (old_rows[i].ws_id)
			chrome_drop_row(&old_rows[i]);
	free(old->cells);
	free(old_rows);
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
	int avail = box.height - (k - 1) * TILE_GAP;

	for (int i = first; i < first + k; i++) {
		ov.cells[i].slot.height = MAX(tile_rest_box(ov.cells[i].c).height, 1);
		sum += ov.cells[i].slot.height;
	}
	for (int i = first; i < first + k; i++) {
		double h = avail * ov.cells[i].slot.height / sum;

		ov.cells[i].slot = (struct nx_box){ box.x, (int)lround(y),
				box.width, (int)lround(y + h) - (int)lround(y) };
		y += h + TILE_GAP;
	}
	return first + k;
}

static int
layout_row(const Frame *f, int r, int first)
{
	Row *row = &ov.rows[r];
	Column *col;
	double total = 0.0, scale, x;
	int cols = 0, h, y;

	row->box = (struct nx_box){ f->lane_x,
			f->top + r * (f->lane_h + LANE_GAP), f->lane_w, f->lane_h };
	wl_list_for_each(col, &row->ws->columns, link) {
		if (!column_shown(col))
			continue;
		total += col->target_width;
		cols++;
	}
	scale = MIN(f->scale, (f->inner_w - (cols - 1) * TILE_GAP) / MAX(total, 1.0));
	scale = MAX(scale, ZOOM_MIN);
	x = f->inner_x + (f->inner_w - total * scale - (cols - 1) * TILE_GAP) / 2.0;
	h = (int)lround(f->mon_h * scale);
	y = row->box.y + (row->box.height - h) / 2;
	wl_list_for_each(col, &row->ws->columns, link) {
		int k = column_shown(col);

		if (!k)
			continue;
		first = layout_column(first, k, (struct nx_box){ (int)lround(x), y,
				(int)lround(col->target_width * scale), h });
		x += col->target_width * scale + TILE_GAP;
	}
	return first;
}

/* Cells and rows from structure. */
int
grid_collect(Monitor *m)
{
	Pool old = { ov.cells, ov.n_cells };
	Row *old_rows = ov.rows;
	int n_old_rows = ov.n_rows, n = 0, rows = 0, first = 0;
	Client *sel = old.n ? old.cells[ov.sel].c : focustop(m);
	Workspace *ws;
	Column *col;
	Frame f;

	wl_list_for_each(ws, &m->workspaces, link) {
		int k = ws_shown(ws);

		n += k;
		rows += k > 0;
	}
	if (!n)
		return 0;

	ov.cells = ecalloc((size_t)n, sizeof(*ov.cells));
	ov.rows = ecalloc((size_t)rows, sizeof(*ov.rows));
	ov.n_cells = ov.n_rows = 0;
	wl_list_for_each(ws, &m->workspaces, link) {
		if (!ws_shown(ws))
			continue;
		workspace_layout(ws);
		ov.rows[ov.n_rows++] = adopt_row(old_rows, n_old_rows, ws);
		wl_list_for_each(col, &ws->columns, link)
			collect_column(col, &old);
	}
	release(&old, old_rows, n_old_rows);

	f = frame_for(m, ov.n_rows);
	for (int r = 0; r < ov.n_rows; r++)
		first = layout_row(&f, r, first);

	ov.sel = 0;
	for (int i = 0; i < ov.n_cells; i++) {
		if (ov.cells[i].c == sel)
			ov.sel = i;
		if (!ov.cells[i].tree)
			ov.cells[i].from = ov.cells[i].slot;
	}
	for (int r = 0; r < ov.n_rows; r++)
		if (!ov.rows[r].lane)
			ov.rows[r].from = ov.rows[r].box;
	return ov.n_cells;
}
