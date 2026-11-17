/* Super+O: every workspace at once. */
#include "overview/state.h"

static const SpringParams SPRING_ZOOM  = { 1.0, 1.0, 1100.0 };
static const SpringParams SPRING_FLOW  = { 1.0, 1.0, 800.0 };
static const SpringParams SPRING_FOCUS = { 1.0, 1.0, 1400.0 };
static const SpringParams SPRING_LIFT  = { 1.0, 0.8, 900.0 };

struct Overview ov;

static void
apply_all(void)
{
	for (int i = 0; i < ov.n_cells; i++)
		cell_apply(&ov.cells[i]);
	chrome_apply();
}

void
overview_redraw(void)
{
	apply_all();
	monitor_wake(ov.mon);
}

void
overview_select(int i)
{
	if (i == ov.sel)
		return;
	ov.ring_from = selection_box();
	ov.focus = (Motion){ 0.0, 0.0, 1.0 };
	ov.sel = i;
	nx_scene_node_raise_to_top(&ov.cells[i].tree->node);
	overview_redraw();
}

static void
overview_teardown(void)
{
	for (int i = 0; i < ov.n_cells; i++)
		snapshot_destroy(ov.cells[i].thumb);
	if (ov.tree)
		nx_scene_node_destroy(&ov.tree->node);
	free(ov.cells);
	free(ov.rows);
	ov = (struct Overview){ 0 };
	detail_drop();
	nx_scene_node_set_enabled(&layers[LyrTile]->node, 1);
	nx_scene_node_set_enabled(&layers[LyrFloat]->node, 1);
}

/* Glide cells to new slots. */
void
overview_reflow(void)
{
	for (int i = 0; i < ov.n_cells; i++)
		ov.cells[i].from = box_lerp(ov.cells[i].from, ov.cells[i].slot,
				ov.flow.pos);
	for (int r = 0; r < ov.n_rows; r++)
		ov.rows[r].from = box_lerp(ov.rows[r].from, ov.rows[r].box,
				ov.flow.pos);
	if (!grid_collect(ov.mon)) {
		overview_teardown();
		return;
	}
	for (int i = 0; i < ov.n_cells; i++) {
		Cell *cell = &ov.cells[i];

		if (cell->tree) {
			cell_refit(cell);
			continue;
		}
		cell->live = cell->slot;
		cell_build(cell);
	}
	chrome_rows();
	nx_scene_node_raise_to_top(&ov.cells[ov.sel].tree->node);
	ov.flow = (Motion){ 0.0, 0.0, 1.0 };
	printstatus();
	overview_redraw();
}

/* Every cell still tiles here. */
static int
intact(void)
{
	for (int i = 0; i < ov.n_cells; i++) {
		Column *col = ov.cells[i].c->column;

		if (!col || col->ws->mon != ov.mon)
			return 0;
	}
	return 1;
}

/* Rebuild when tiles left. */
int
overview_revalidate(void)
{
	if (!intact())
		overview_reflow();
	return ov.mon != NULL;
}

static struct nx_box
content_box(Client *c)
{
	int bw = (int)c->bw;

	return (struct nx_box){ c->geom.x + bw, c->geom.y + bw,
			c->geom.width - 2 * bw, c->geom.height - 2 * bw };
}

static void
overview_open(Monitor *m)
{
	Workspace *active = m ? monitor_active_ws(m) : NULL;

	if (!active || !grid_collect(m))
		return;
	ov.mon = m;
	for (int i = 0; i < ov.n_cells; i++) {
		Client *c = ov.cells[i].c;

		ov.cells[i].live = c->column->ws == active
				? content_box(c) : grid_screen_box(c, active);
	}
	chrome_build();
	for (int i = 0; i < ov.n_cells; i++)
		cell_build(&ov.cells[i]);
	chrome_rows();
	nx_scene_node_raise_to_top(&ov.cells[ov.sel].tree->node);
	ov.zoom = (Motion){ 0.0, 0.0, 1.0 };
	ov.flow = ov.focus = (Motion){ 1.0, 0.0, 1.0 };
	ov.ring_from = ov.cells[ov.sel].slot;
	/* Thumbnails replace real windows. */
	nx_scene_node_set_enabled(&layers[LyrTile]->node, 0);
	nx_scene_node_set_enabled(&layers[LyrFloat]->node, 0);
	detail_fetch();
	overview_redraw();
}

/* Zoom into the landing spots. */
static void
begin_close(Client *target)
{
	Workspace *dest = target ? target->column->ws : monitor_active_ws(ov.mon);

	ov.closing = 1;
	ov.zoom.target = 0.0;
	ov.activate = target;
	if (target) {
		dest->focused_col = target->column;
		workspace_layout(dest);
	}
	for (int i = 0; i < ov.n_cells; i++)
		ov.cells[i].live = grid_screen_box(ov.cells[i].c, dest);
	monitor_wake(ov.mon);
}

void
overview_cancel(void)
{
	if (!ov.closing && overview_revalidate())
		begin_close(NULL);
}

void
overview_choose(void)
{
	if (!ov.closing && overview_revalidate())
		begin_close(ov.cells[ov.sel].c);
}

/* Hidden springs jump to rest. */
static void
settle(Monitor *m)
{
	Workspace *ws;
	Column *col;

	wl_list_for_each(ws, &m->workspaces, link) {
		wl_list_for_each(col, &ws->columns, link)
			col->just_created = 1;
		workspace_layout(ws);
		ws->scroll_x = ws->target_scroll_x;
		ws->scroll_x_f = ws->target_scroll_x;
		ws->scroll_x_vel = 0.0;
	}
}

static int
on_active(Client *c, Monitor *m)
{
	Workspace *ws;

	if (!c)
		return 0;
	ws = c->column ? c->column->ws : client_float_ws(c);
	return !ws || ws == m->active_ws;
}

static void
overview_finish_close(void)
{
	Monitor *m = ov.mon;
	Client *target = ov.activate;
	Workspace *dest = target && target->column
			? target->column->ws : monitor_active_ws(m);

	overview_teardown();
	settle(m);
	if (dest != m->active_ws) {
		workspace_switch(m, dest);
		m->ws_y_offset = m->ws_y_vel = 0.0;
	}
	arrange(m);
	if (target)
		focusclient(target, 1);
	else if (!on_active(focustop(m), m))
		focus_first_in_workspace(m->active_ws);
	printstatus();
}

int
overview_is_open(void)
{
	return ov.mon != NULL;
}

void
overview_toggle(const Arg *arg)
{
	(void)arg;
	if (ov.mon)
		overview_cancel();
	else
		overview_open(selmon);
}

static int
motion_tick(Motion *mo, SpringParams sp, double dt)
{
	return spring_tick(&mo->pos, &mo->vel, mo->target, sp, dt);
}

void
overview_tick(Monitor *m, double dt, int *still)
{
	int moved;

	if (m != ov.mon)
		return;
	moved = motion_tick(&ov.zoom, SPRING_ZOOM, dt);
	moved |= motion_tick(&ov.flow, SPRING_FLOW, dt);
	moved |= motion_tick(&ov.focus, SPRING_FOCUS, dt);
	for (int i = 0; i < ov.n_cells; i++) {
		ov.cells[i].lift.target = i == ov.sel && !ov.closing;
		moved |= motion_tick(&ov.cells[i].lift, SPRING_LIFT, dt);
	}
	if (moved) {
		apply_all();
		*still = 1;
		return;
	}
	if (ov.closing)
		overview_finish_close();
}

void
overview_purge_mon(Monitor *m)
{
	if (ov.mon == m)
		overview_teardown();
}

/* A dying tile ends it. */
void
overview_purge_client(Client *c)
{
	for (int i = 0; i < ov.n_cells; i++) {
		if (ov.cells[i].c != c)
			continue;
		overview_teardown();
		return;
	}
}
