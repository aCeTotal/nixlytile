/* Mouse in the overview. */
#include "overview/state.h"

#include <float.h>

#define DRAG_START_PX 6

typedef struct {
	int held, cell, row;
	double x, y, fx, fy;
} Press;

static Press press;

static int
inside(struct nx_box b, double x, double y)
{
	return x >= b.x && y >= b.y && x < b.x + b.width && y < b.y + b.height;
}

/* Topmost cell under the point. */
static int
cell_at(double x, double y)
{
	if (inside(cell_box(&ov.cells[ov.sel]), x, y))
		return ov.sel;
	for (int i = 0; i < ov.n_cells; i++)
		if (inside(cell_box(&ov.cells[i]), x, y))
			return i;
	return -1;
}

static int
row_at(double x, double y)
{
	for (int r = 0; r < ov.n_rows; r++)
		if (inside(ov.rows[r].box, x, y))
			return r;
	return -1;
}

/* Nearest window in row r. */
static int
cell_near(int r, double x)
{
	double best_d = DBL_MAX;
	int best = -1;

	for (int i = 0; i < ov.n_cells; i++) {
		struct nx_box b = ov.cells[i].slot;
		double d = fabs(x - (b.x + b.width / 2.0));

		if (ov.cells[i].c->column->ws != ov.rows[r].ws || d >= best_d)
			continue;
		best_d = d;
		best = i;
	}
	return best;
}

static void
choose(int i)
{
	if (i < 0)
		return;
	overview_select(i);
	overview_choose();
}

static void
release(double x, double y)
{
	press.held = 0;
	if (ov.drag.active) {
		drag_drop();
		overview_pointer_motion(x, y);
		return;
	}
	if (press.cell >= 0 && cell_at(x, y) == press.cell) {
		choose(press.cell);
		return;
	}
	if (press.row >= 0 && row_at(x, y) == press.row) {
		choose(cell_near(press.row, x));
		return;
	}
	if (press.cell < 0 && press.row < 0)
		overview_cancel();
}

static void
grab(double x, double y)
{
	struct nx_box b;

	press = (Press){ 1, cell_at(x, y), row_at(x, y), x, y, 0.0, 0.0 };
	if (press.cell < 0)
		return;
	b = cell_box(&ov.cells[press.cell]);
	press.fx = (x - b.x) / MAX(b.width, 1);
	press.fy = (y - b.y) / MAX(b.height, 1);
}

/* Only releases of own presses. */
int
overview_pointer_button(uint32_t button, int pressed)
{
	if (!ov.mon || (!pressed && !press.held))
		return 0;
	if (ov.closing || button != BTN_LEFT || !overview_revalidate())
		return 1;
	if (pressed)
		grab(cursor->x, cursor->y);
	else if (press.held)
		release(cursor->x, cursor->y);
	return 1;
}

static int
drag_due(double x, double y)
{
	return press.held && press.cell >= 0 && !ov.drag.active
			&& hypot(x - press.x, y - press.y) > DRAG_START_PX;
}

void
overview_pointer_motion(double x, double y)
{
	int i;

	if (!ov.mon || ov.closing || !overview_revalidate())
		return;
	if (drag_due(x, y))
		drag_start(press.cell, press.fx, press.fy);
	if (ov.drag.active) {
		drag_follow(x, y);
		return;
	}
	i = cell_at(x, y);
	if (i >= 0 && !press.held)
		overview_select(i);
	nixly_cursor_set_xcursor(i >= 0 ? "pointer" : "default");
}

void
pointer_reset(void)
{
	press.held = 0;
	if (ov.drag.active)
		drag_settle();
	ghost_hide();
}
