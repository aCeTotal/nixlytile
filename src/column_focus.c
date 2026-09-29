#include "nixlytile.h"
#include "client.h"

#include <math.h>

/* Focus target at a position. */
typedef struct {
	Client *c;
	double x, y;
} Stop;

typedef struct {
	Stop from;
	int dir;
	Stop best;
} Seek;

/* Settled screen box. */
static struct wlr_box
column_box(const Workspace *ws, const Column *col)
{
	return (struct wlr_box){
		.x = ws->mon->w.x + col->target_x - ws->target_scroll_x,
		.y = ws->mon->w.y,
		.width = col->target_width,
		.height = col->target_height,
	};
}

static Stop
column_stop(const Workspace *ws, Column *col)
{
	struct wlr_box box = column_box(ws, col);
	Client *c = wl_container_of(col->clients.next, c, column_link);

	return (Stop){ c, box.x + box.width / 2.0, box.y + box.height / 2.0 };
}

static Stop
floating_stop(Client *c)
{
	return (Stop){ c, c->geom.x + c->geom.width / 2.0,
		c->geom.y + c->geom.height / 2.0 };
}

static Stop
edge_stop(int dir)
{
	return (Stop){ NULL, dir > 0 ? -INFINITY : INFINITY, 0.0 };
}

/* Floating window clear of tiles. */
static int
floats_free(Monitor *m, Client *c)
{
	Workspace *ws = m->active_ws;
	Column *col;

	if (!c->isfloating || c->isfullscreen || client_is_unmanaged(c)
			|| !VISIBLEON(c, m) || !c->scene->node.enabled)
		return 0;
	if (!ws)
		return 1;
	wl_list_for_each(col, &ws->columns, link) {
		struct wlr_box box = column_box(ws, col);

		if (wlr_box_intersects(&box, &c->geom))
			return 0;
	}
	return 1;
}

/* Left to right, then top down. */
static int
beyond(const Stop *a, const Stop *b, int dir)
{
	double dx = dir * (b->x - a->x);

	return dx > 0 || (dx == 0 && dir * (b->y - a->y) > 0);
}

static void
seek_offer(Seek *s, Stop cand)
{
	if (!beyond(&s->from, &cand, s->dir))
		return;
	if (!s->best.c || beyond(&cand, &s->best, s->dir))
		s->best = cand;
}

/* Nearest stop past from. */
static Client *
seek_stop(Monitor *m, Stop from, int dir)
{
	Seek s = { .from = from, .dir = dir };
	Workspace *ws = m->active_ws;
	Column *col;
	Client *c;

	wl_list_for_each(c, &clients, link) {
		if (floats_free(m, c))
			seek_offer(&s, floating_stop(c));
	}
	if (!ws)
		return s.best.c;
	wl_list_for_each(col, &ws->columns, link) {
		if (!wl_list_empty(&col->clients))
			seek_offer(&s, column_stop(ws, col));
	}
	return s.best.c;
}

static Stop
current_stop(Monitor *m, int dir)
{
	Client *c = focustop(m);
	Workspace *ws = m->active_ws;

	if (c && floats_free(m, c))
		return floating_stop(c);
	if (ws && ws->focused_col && !wl_list_empty(&ws->focused_col->clients))
		return column_stop(ws, ws->focused_col);
	return edge_stop(dir);
}

static Monitor *
adjacent_mon(Monitor *m, int dir)
{
	struct wlr_output *out = wlr_output_layout_adjacent_output(output_layout,
			dir > 0 ? WLR_DIRECTION_RIGHT : WLR_DIRECTION_LEFT,
			m->wlr_output, m->m.x + m->m.width / 2.0,
			m->m.y + m->m.height / 2.0);
	Monitor *next = out ? out->data : NULL;

	return next && next->wlr_output->enabled ? next : NULL;
}

static void
land(Monitor *m, Client *c)
{
	int moved = m != selmon;

	selmon = m;
	if (c->column) {
		c->column->ws->focused_col = c->column;
		/* Warp reads fresh targets. */
		arrange(m);
	}
	focusclient(c, 1);
	if (moved)
		printstatus();
}

static void
land_empty(Monitor *m)
{
	selmon = m;
	wlr_cursor_warp(cursor, NULL, m->m.x + m->m.width / 2.0,
			m->m.y + m->m.height / 2.0);
	focusclient(focustop(m), 0);
	motionnotify(0, NULL, 0, 0, 0, 0);
	printstatus();
}

void
focus_column_dir(const Arg *arg)
{
	int dir = arg->i > 0 ? 1 : -1;
	Monitor *next;
	Client *c;

	if (!selmon || !selmon->active_ws)
		return;
	c = seek_stop(selmon, current_stop(selmon, dir), dir);
	if (c) {
		land(selmon, c);
		return;
	}
	next = adjacent_mon(selmon, dir);
	if (!next)
		return;
	c = seek_stop(next, edge_stop(dir), dir);
	if (!c) {
		land_empty(next);
		return;
	}
	land(next, c);
}
