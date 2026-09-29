/*
 * wobble.c — jelly drag for floating windows.
 *
 * While a floating window is dragged, its surface is hidden and shown
 * as horizontal strips of the same buffer instead.  Each strip edge is
 * a damped spring: edges near the grab point follow the cursor almost
 * rigidly, distant edges lag and swing back, so the window bends and
 * settles like jelly.  Pure scene graph (source box + dest size per
 * strip), no renderer work; the strips are dropped once it is still.
 */
#include "nixlytile.h"
#include "client.h"

#include <math.h>
#include <stdlib.h>

#define WOBBLE_STRIP_PX   8      /* target strip height */
#define WOBBLE_MIN_STRIPS 16
#define WOBBLE_MAX_STRIPS 128
#define WOBBLE_STIFFNESS  320.0  /* spring constant at the grab edge */
#define WOBBLE_LOOSEST    0.22   /* stiffness share at the far edge */
#define WOBBLE_DAMPING    0.32   /* damping ratio; below 1 overshoots */
#define WOBBLE_MAX_LAG    0.30   /* offset cap, share of window height */
#define WOBBLE_REST_PX    0.3
#define WOBBLE_REST_VEL   6.0
#define WOBBLE_MAX_DT     0.033

struct Wobble {
	struct wlr_scene_tree *tree;
	struct wlr_scene_buffer **strips;
	uint32_t seq;                /* commit the strips show */
	int shown;
	double *x, *y, *vx, *vy;     /* per strip edge, n + 1 each */
	int n;
	int grab;                    /* edge pinned under the cursor */
	int held;
	int last_x, last_y;
	int border_enabled[4];
};

static int
wobble_eligible(struct wlr_surface *surf)
{
	/* Only the root buffer is stripped; subsurface content (video,
	 * GL children) would vanish mid-drag, so those move rigidly. */
	return surf && surf->buffer
		&& wl_list_empty(&surf->current.subsurfaces_above)
		&& wl_list_empty(&surf->current.subsurfaces_below);
}

static void
wobble_hide_client(Client *c, Wobble *w, int hidden)
{
	int i;

	wlr_scene_node_set_enabled(&c->scene_surface->node, !hidden);
	for (i = 0; i < 4; i++) {
		if (hidden)
			w->border_enabled[i] = c->border[i]->node.enabled;
		wlr_scene_node_set_enabled(&c->border[i]->node,
				!hidden && w->border_enabled[i]);
	}
}

static Wobble *
wobble_create(Client *c, int height)
{
	Wobble *w = ecalloc(1, sizeof(*w));
	int i;

	w->n = MAX(WOBBLE_MIN_STRIPS, MIN(WOBBLE_MAX_STRIPS, height / WOBBLE_STRIP_PX));
	w->strips = ecalloc((size_t)w->n, sizeof(*w->strips));
	w->x = ecalloc(4 * (size_t)(w->n + 1), sizeof(double));
	w->y = w->x + w->n + 1;
	w->vx = w->y + w->n + 1;
	w->vy = w->vx + w->n + 1;
	w->tree = wlr_scene_tree_create(c->scene);
	for (i = 0; i < w->n; i++)
		w->strips[i] = wlr_scene_buffer_create(w->tree, NULL);
	wobble_hide_client(c, w, 1);
	return w;
}

void
wobble_grab(Client *c, double cursor_y)
{
	struct wlr_surface *surf = client_surface(c);
	int height = c->geom.height - 2 * (int)c->bw;
	Wobble *w;

	if (height <= 0 || !wobble_eligible(surf))
		return;
	if (!c->wobble)
		c->wobble = wobble_create(c, height);
	w = c->wobble;
	w->held = 1;
	w->grab = (int)lround((cursor_y - c->geom.y - c->bw) / height * w->n);
	w->grab = MAX(0, MIN(w->n, w->grab));
	w->last_x = c->geom.x;
	w->last_y = c->geom.y;
	if (c->mon && c->mon->wlr_output)
		wlr_output_schedule_frame(c->mon->wlr_output);
}

void
wobble_release(Client *c)
{
	if (c->wobble)
		c->wobble->held = 0;
}

void
wobble_stop(Client *c)
{
	Wobble *w = c->wobble;

	if (!w)
		return;
	wlr_scene_node_destroy(&w->tree->node);
	wobble_hide_client(c, w, 0);
	free(w->strips);
	free(w->x);
	free(w);
	c->wobble = NULL;
}

static void
wobble_spring(Wobble *w, int edge, double dx, double dy, double dt)
{
	double near = 1.0 - fabs((double)(edge - w->grab)) / w->n;
	double k = WOBBLE_STIFFNESS * (WOBBLE_LOOSEST + (1.0 - WOBBLE_LOOSEST) * near);
	double damping = 2.0 * WOBBLE_DAMPING * sqrt(k);
	double cap = WOBBLE_MAX_LAG * w->n * WOBBLE_STRIP_PX;

	if (w->held && edge == w->grab) {
		w->x[edge] = w->y[edge] = w->vx[edge] = w->vy[edge] = 0.0;
		return;
	}
	/* The window moved by (dx, dy); loose edges stay behind. */
	w->x[edge] -= dx * (1.0 - near);
	w->y[edge] -= dy * (1.0 - near);
	w->vx[edge] += (-k * w->x[edge] - damping * w->vx[edge]) * dt;
	w->vy[edge] += (-k * w->y[edge] - damping * w->vy[edge]) * dt;
	w->x[edge] = fmax(-cap, fmin(cap, w->x[edge] + w->vx[edge] * dt));
	w->y[edge] = fmax(-cap, fmin(cap, w->y[edge] + w->vy[edge] * dt));
}

static struct wlr_fbox
wobble_source(Client *c, struct wlr_surface *surf, struct wlr_buffer *buf)
{
	struct wlr_fbox whole = { 0, 0, buf->width, buf->height };
	struct wlr_box g;
	double sx, sy;

	if (client_is_x11(c) || surf->current.width <= 0 || surf->current.height <= 0)
		return whole;
	client_get_geometry(c, &g);
	if (g.width <= 0 || g.height <= 0)
		return whole;
	sx = (double)buf->width / surf->current.width;
	sy = (double)buf->height / surf->current.height;
	return (struct wlr_fbox){ g.x * sx, g.y * sy, g.width * sx, g.height * sy };
}

static void
wobble_layout(Client *c, struct wlr_surface *surf)
{
	Wobble *w = c->wobble;
	struct wlr_buffer *buf = &surf->buffer->base;
	struct wlr_fbox src = wobble_source(c, surf, buf);
	int bw = (int)c->bw;
	int width = c->geom.width - 2 * bw;
	int height = c->geom.height - 2 * bw;
	int refresh = !w->shown || surf->current.seq != w->seq;
	int i;

	for (i = 0; i < w->n; i++) {
		struct wlr_scene_buffer *strip = w->strips[i];
		int top = (int)lround((double)height * i / w->n + w->y[i]);
		int bottom = (int)lround((double)height * (i + 1) / w->n + w->y[i + 1]);
		struct wlr_fbox slice = {
			src.x, src.y + src.height * i / w->n,
			src.width, src.height / w->n,
		};

		if (refresh)
			wlr_scene_buffer_set_buffer(strip, buf);
		wlr_scene_buffer_set_source_box(strip, &slice);
		wlr_scene_buffer_set_dest_size(strip, width, MAX(1, bottom - top));
		wlr_scene_node_set_position(&strip->node,
				bw + (int)lround((w->x[i] + w->x[i + 1]) / 2.0), bw + top);
	}
	w->seq = surf->current.seq;
	w->shown = 1;
}

static int
wobble_settled(const Wobble *w)
{
	int i;

	if (w->held)
		return 0;
	for (i = 0; i <= w->n; i++) {
		if (fabs(w->x[i]) > WOBBLE_REST_PX || fabs(w->y[i]) > WOBBLE_REST_PX
				|| fabs(w->vx[i]) > WOBBLE_REST_VEL
				|| fabs(w->vy[i]) > WOBBLE_REST_VEL)
			return 0;
	}
	return 1;
}

/* Returns 1 while the client still wobbles. */
static int
wobble_step(Client *c, double dt)
{
	Wobble *w = c->wobble;
	struct wlr_surface *surf = client_surface(c);
	double dx = c->geom.x - w->last_x;
	double dy = c->geom.y - w->last_y;
	int i;

	if (!wobble_eligible(surf))
		return 0;
	w->last_x = c->geom.x;
	w->last_y = c->geom.y;
	for (i = 0; i <= w->n; i++)
		wobble_spring(w, i, dx, dy, dt);
	if (wobble_settled(w))
		return 0;
	wobble_layout(c, surf);
	return 1;
}

void
wobble_tick(Monitor *m, double dt, int *still)
{
	Client *c;

	*still = 0;
	dt = MIN(dt, WOBBLE_MAX_DT);
	wl_list_for_each(c, &clients, link) {
		if (!c->wobble || c->mon != m)
			continue;
		if (wobble_step(c, dt))
			*still = 1;
		else
			wobble_stop(c);
	}
}
