/* Dragging a window between slots. */
#include "overview/state.h"

#include <float.h>

#define DRAG_GROW 1.06
#define MARKER_W  4

static double
centre_x(struct nx_box b)
{
	return b.x + b.width / 2.0;
}

static struct nx_box
lane(int r)
{
	return r < ov.n_rows ? ov.rows[r].box : ov.ghost.box;
}

static int
row_near(double y)
{
	double best_d = DBL_MAX;
	int best = 0;

	for (int r = 0; r < ov.n_rows + ov.ghost.shown; r++) {
		struct nx_box b = lane(r);
		double d = fabs(y - (b.y + b.height / 2.0));

		if (d >= best_d)
			continue;
		best_d = d;
		best = r;
	}
	return best;
}

/* Column's first cell in r. */
static int
is_head(int i, int r)
{
	Column *col = ov.cells[i].c->column;
	int alone;

	if (!col)
		return 0;
	alone = col == ov.cells[ov.drag.cell].c->column && col->n_clients == 1;
	return col->ws == ov.rows[r].ws && !alone
			&& (i == 0 || ov.cells[i - 1].c->column != col);
}

/* Row r's k-th column head. */
static int
nth_column(int r, int k)
{
	for (int i = 0; i < ov.n_cells; i++)
		if (is_head(i, r) && k-- == 0)
			return i;
	return -1;
}

/* Columns left of x. */
static int
column_slot(int r, double x)
{
	int k = 0;

	for (int i = 0; i < ov.n_cells; i++)
		k += is_head(i, r) && centre_x(ov.cells[i].slot) < x;
	return k;
}

struct nx_box
drag_box(void)
{
	const Cell *cell = &ov.cells[ov.drag.cell];
	struct nx_box s = box_lerp(cell->from, cell->slot, ov.flow.pos);
	int w = (int)lround(s.width * DRAG_GROW);
	int h = (int)lround(s.height * DRAG_GROW);

	return (struct nx_box){ (int)lround(ov.drag.x - ov.drag.ox * w),
			(int)lround(ov.drag.y - ov.drag.oy * h), w, h };
}

/* Insertion bar between columns. */
struct nx_box
drag_marker(void)
{
	struct nx_box lane = ov.rows[ov.drag.row].box;
	int prev = nth_column(ov.drag.row, ov.drag.col - 1);
	int next = nth_column(ov.drag.row, ov.drag.col);
	double x = lane.x + lane.width / 2.0;

	if (prev >= 0 && next >= 0)
		x = (ov.cells[prev].slot.x + ov.cells[prev].slot.width
				+ ov.cells[next].slot.x) / 2.0;
	else if (prev >= 0)
		x = ov.cells[prev].slot.x + ov.cells[prev].slot.width + OV_TILE_GAP / 2.0;
	else if (next >= 0)
		x = ov.cells[next].slot.x - OV_TILE_GAP / 2.0;
	return (struct nx_box){ (int)lround(x - MARKER_W / 2.0),
			lane.y + OV_LANE_PAD, MARKER_W, lane.height - 2 * OV_LANE_PAD };
}

void
drag_start(int cell, double fx, double fy)
{
	overview_select(cell);
	ov.drag.active = 1;
	ov.drag.cell = cell;
	ov.drag.ox = fx;
	ov.drag.oy = fy;
	ghost_show();
}

void
drag_follow(double x, double y)
{
	ov.drag.x = x;
	ov.drag.y = y;
	ov.drag.row = row_near(y);
	ov.drag.col = ov.drag.row < ov.n_rows ? column_slot(ov.drag.row, x) : 0;
	nixly_cursor_set_xcursor("grabbing");
	cell_apply(&ov.cells[ov.drag.cell]);
	chrome_apply();
	monitor_wake(ov.mon);
}

/* Thumbnail stays where it hangs. */
void
drag_settle(void)
{
	Cell *cell = &ov.cells[ov.drag.cell];

	cell->slot = cell->from = drag_box();
	ov.drag.active = 0;
}

static Column *
column_of(int cell)
{
	return cell < 0 ? NULL : ov.cells[cell].c->column;
}

/* Column index, skip removed. */
static int
index_without(Column *col, Column *skip)
{
	int i = column_index(col);

	return i - (column_index(skip) < i);
}

static int
target_index(Column *moving, Column *prev, Column *next)
{
	if (next)
		return index_without(next, moving);
	if (prev)
		return index_without(prev, moving) + 1;
	return 0;
}

static void
move_tile(Client *c, Workspace *dst)
{
	workspace_detach_client(c);
	workspace_attach_client(dst, c);
}

/* Own column at the marker. */
static void
insert_at_marker(Client *c)
{
	Workspace *dst = ov.rows[ov.drag.row].ws;
	Column *prev = column_of(nth_column(ov.drag.row, ov.drag.col - 1));
	Column *next = column_of(nth_column(ov.drag.row, ov.drag.col));

	if (c->column->ws != dst || c->column->n_clients > 1)
		move_tile(c, dst);
	column_move_to_index(c->column, target_index(c->column, prev, next));
}

void
drag_drop(void)
{
	Client *c = ov.cells[ov.drag.cell].c;

	if (ov.drag.row < ov.n_rows)
		insert_at_marker(c);
	else
		move_tile(c, ghost_workspace());
	monitor_compact_workspaces(ov.mon);
	overview_reflow();
}
