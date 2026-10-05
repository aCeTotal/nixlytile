#include <stdlib.h>
#include <string.h>

#include "nixlytile.h"
#include "client.h"
#include "remote.h"

static int
in_workspace(Client *c, Workspace *ws)
{
	return c->column ? c->column->ws == ws : client_float_ws(c) == ws;
}

/* Move ws, floats keep their offset. */
static void
carry_workspace(Workspace *ws, Monitor *dst, const struct wlr_box *from)
{
	Client *c;

	workspace_move_to_monitor(ws, dst);
	wl_list_for_each(c, &clients, link) {
		if (client_float_ws(c) != ws)
			continue;
		resize(c, (struct wlr_box){
			.x = c->geom.x + dst->m.x - from->x,
			.y = c->geom.y + dst->m.y - from->y,
			.width = c->geom.width, .height = c->geom.height}, 0);
	}
}

/* Park m's populated workspaces on dst. */
void
monitor_evacuate_workspaces(Monitor *m, Monitor *dst)
{
	Workspace *ws, *tmp;

	if (!dst)
		return;
	wl_list_for_each_safe(ws, tmp, &m->workspaces, link) {
		if (workspace_has_clients(ws))
			carry_workspace(ws, dst, &m->m);
	}
	if (wl_list_empty(&m->workspaces))
		m->active_ws = workspace_create(m);
	monitor_compact_workspaces(dst);
}

static int
reclaim_from(Monitor *m, Monitor *src)
{
	Workspace *ws, *tmp;
	Client *c;
	int moved = 0;

	wl_list_for_each_safe(ws, tmp, &src->workspaces, link) {
		if (strcmp(ws->home, m->wlr_output->name) || !workspace_has_clients(ws))
			continue;
		carry_workspace(ws, m, &src->m);
		wl_list_for_each(c, &clients, link) {
			if (!in_workspace(c, ws))
				continue;
			free(c->output);
			if (!(c->output = strdup(m->wlr_output->name)))
				die("oom");
		}
		moved = 1;
	}
	if (!moved)
		return 0;
	if (wl_list_empty(&src->workspaces))
		src->active_ws = workspace_create(src);
	monitor_compact_workspaces(src);
	return 1;
}

/* Bring m's parked workspaces back home. */
void
monitor_reclaim_workspaces(Monitor *m)
{
	Monitor *src;
	int moved = 0;

	if (REMOTE_PARKED(m))
		return;
	/* Floats clamp against m->w. */
	wlr_output_layout_get_box(output_layout, m->wlr_output, &m->m);
	m->w = m->m;
	wl_list_for_each(src, &mons, link) {
		if (src != m)
			moved |= reclaim_from(m, src);
	}
	if (moved)
		monitor_compact_workspaces(m);
}
