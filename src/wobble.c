#include "nixlytile.h"
#include "client.h"
#include "wobble.h"

#include <math.h>
#include <stdlib.h>

#define WOBBLE_STIFFNESS  1000.0 /* spring constant at the grab edge */
#define WOBBLE_LOOSEST    0.40   /* stiffness share at the far edge */
#define WOBBLE_DAMPING    0.38   /* damping ratio; below 1 overshoots */
#define WOBBLE_MAX_LAG    0.25   /* offset cap, share of window height */
#define WOBBLE_STEP       (1.0 / 240.0)
#define WOBBLE_MAX_DT     0.033
#define WOBBLE_REST_PX    0.5
#define WOBBLE_REST_VEL   12.0

enum WobbleState { WobbleDone, WobbleIdle, WobbleMoving };

static int
wobble_eligible(Client *c)
{
	struct wlr_surface *surf = client_surface(c);

	/* Only content the view reproduces. */
	return c->isfloating && !c->isfullscreen && !c->frozen_buffer
		&& surf && surf->buffer && surf->buffer->texture
		&& surf->current.transform == WL_OUTPUT_TRANSFORM_NORMAL
		&& !wlr_surface_get_image_description_v1_data(surf)
		&& wl_list_empty(&surf->current.subsurfaces_above)
		&& wl_list_empty(&surf->current.subsurfaces_below);
}

static bool
wobble_rejects_input(struct wlr_scene_buffer *buffer, double *sx, double *sy)
{
	return false;
}

static void
wobble_find_real(struct wlr_scene_buffer *buffer, int sx, int sy, void *out)
{
	*(struct wlr_scene_buffer **)out = buffer;
}

static float
wobble_alpha(struct wlr_surface *surf)
{
	const struct wlr_alpha_modifier_surface_v1_state *state =
		wlr_alpha_modifier_v1_get_surface_state(surf);

	return state ? (float)state->multiplier : 1.0f;
}

/* Runs after wlroots resets opacity. */
static void
wobble_commit(struct wl_listener *listener, void *data)
{
	Wobble *w = wl_container_of(listener, w, commit);

	if (w->concealed)
		wlr_scene_buffer_set_opacity(w->real, 0.0f);
}

static void
wobble_conceal(Client *c, Wobble *w)
{
	int i;

	wlr_scene_buffer_set_opacity(w->real, 0.0f);
	if (w->concealed)
		return;
	for (i = 0; i < 4; i++) {
		w->border_enabled[i] = c->border[i]->node.enabled;
		wlr_scene_node_set_enabled(&c->border[i]->node, 0);
	}
	w->concealed = 1;
}

static void
wobble_reveal(Client *c, Wobble *w)
{
	int i;

	if (!w->concealed)
		return;
	wlr_scene_buffer_set_opacity(w->real, wobble_alpha(client_surface(c)));
	for (i = 0; i < 4; i++)
		wlr_scene_node_set_enabled(&c->border[i]->node, w->border_enabled[i]);
}

static Wobble *
wobble_create(Client *c, struct wlr_scene_buffer *real)
{
	Wobble *w = ecalloc(1, sizeof(*w));

	w->real = real;
	w->view = wlr_scene_buffer_create(c->scene, NULL);
	w->view->point_accepts_input = wobble_rejects_input;
	wlr_scene_node_place_below(&w->view->node, &c->scene_surface->node);
	w->commit.notify = wobble_commit;
	wl_signal_add(&client_surface(c)->events.commit, &w->commit);
	return w;
}

void
wobble_grab(Client *c, double cursor_y)
{
	int height = c->geom.height - 2 * (int)c->bw;
	struct wlr_scene_buffer *real = NULL;
	Wobble *w;

	if (height <= 0 || !wobble_eligible(c))
		return;
	wlr_scene_node_for_each_buffer(&c->scene_surface->node, wobble_find_real, &real);
	if (!real)
		return;
	if (!c->wobble)
		c->wobble = wobble_create(c, real);
	w = c->wobble;
	w->held = 1;
	w->grab = (int)lround((cursor_y - c->geom.y - c->bw) / height * WOBBLE_EDGES);
	w->grab = MAX(0, MIN(WOBBLE_EDGES, w->grab));
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
	wobble_reveal(c, w);
	wl_list_remove(&w->commit.link);
	wlr_scene_node_destroy(&w->view->node);
	wlr_swapchain_destroy(w->swapchain);
	free(w);
	c->wobble = NULL;
}

/* Loose edges lag the move. */
static void
wobble_substep(Wobble *w, const double moved[2], double h)
{
	int e, a;

	for (e = 0; e <= WOBBLE_EDGES; e++) {
		double loose = fabs((double)(e - w->grab)) / WOBBLE_EDGES;
		double k = WOBBLE_STIFFNESS * (1.0 - (1.0 - WOBBLE_LOOSEST) * loose);
		double damping = 2.0 * WOBBLE_DAMPING * sqrt(k);

		for (a = 0; a < 2; a++) {
			double *off = &w->off[a][e], *vel = &w->vel[a][e];

			*off -= moved[a] * loose;
			*vel += (-k * *off - damping * *vel) * h;
			*off = fmax(-w->cap, fmin(w->cap, *off + *vel * h));
		}
	}
}

/* Fixed steps keep every refresh rate alike. */
static void
wobble_integrate(Wobble *w, const double moved[2], double dt)
{
	int steps = MAX(1, (int)ceil(dt / WOBBLE_STEP));
	double part[2] = { moved[0] / steps, moved[1] / steps };
	int i;

	for (i = 0; i < steps; i++)
		wobble_substep(w, part, dt / steps);
}

static int
wobble_resting(const Wobble *w)
{
	const double *off = &w->off[0][0], *vel = &w->vel[0][0];
	int i;

	for (i = 0; i < 2 * (WOBBLE_EDGES + 1); i++) {
		if (fabs(off[i]) > WOBBLE_REST_PX || fabs(vel[i]) > WOBBLE_REST_VEL)
			return 0;
	}
	return 1;
}

static enum WobbleState
wobble_step(Client *c, double dt)
{
	Wobble *w = c->wobble;
	double moved[2] = { c->geom.x - w->last_x, c->geom.y - w->last_y };
	int resting;

	if (!wobble_eligible(c))
		return WobbleDone;
	w->last_x = c->geom.x;
	w->last_y = c->geom.y;
	w->cap = WOBBLE_MAX_LAG * (c->geom.height - 2 * (int)c->bw);
	wobble_integrate(w, moved, dt);
	resting = wobble_resting(w);
	if (resting && !w->held)
		return WobbleDone;
	if (resting && w->concealed && client_surface(c)->current.seq == w->seq)
		return WobbleIdle;
	if (!wobble_render(c, w))
		return WobbleDone;
	wlr_scene_buffer_set_opacity(w->view, wobble_alpha(client_surface(c)));
	wobble_conceal(c, w);
	w->seq = client_surface(c)->current.seq;
	return WobbleMoving;
}

void
wobble_tick(Monitor *m, double dt, int *still)
{
	Client *c;
	enum WobbleState state;

	*still = 0;
	dt = MIN(dt, WOBBLE_MAX_DT);
	wl_list_for_each(c, &clients, link) {
		if (!c->wobble || c->mon != m)
			continue;
		state = wobble_step(c, dt);
		if (state == WobbleDone)
			wobble_stop(c);
		*still |= state == WobbleMoving;
	}
}
