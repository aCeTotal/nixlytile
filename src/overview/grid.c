/* Cells and rows from workspaces. */
#include "overview/state.h"

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

int
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

static int
ws_alive(const Row *row)
{
	Workspace *ws;

	wl_list_for_each(ws, &ov.mon->workspaces, link)
		if (ws == row->ws && ws->window_id == row->ws_id)
			return 1;
	return 0;
}

/* Rows name live workspaces. */
int
grid_rows_alive(void)
{
	for (int r = 0; r < ov.n_rows; r++)
		if (!ws_alive(&ov.rows[r]))
			return 0;
	return 1;
}

/* Cells and rows from structure. */
int
grid_collect(Monitor *m)
{
	Pool old = { ov.cells, ov.n_cells };
	Row *old_rows = ov.rows;
	int n_old_rows = ov.n_rows, n = 0, rows = 0;
	Client *sel = old.n ? old.cells[ov.sel].c : focustop(m);
	Workspace *ws;
	Column *col;

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

	grid_layout(m);

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
