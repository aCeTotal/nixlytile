#include "nixlytile.h"
#include "client.h"

/*
 * Apply a (sx, sy) visual scale to every scene_buffer beneath a
 * client's surface tree.  Each buffer's dest_size is set to
 * natural × scale, so wlroots renders the existing buffer scaled to
 * the desired box without waiting for the client to commit a new
 * buffer at the new size.
 *
 * Used during column-fullscreen / size transitions so that the
 * window CONTENT scales smoothly in realtime even before the client
 * has acknowledged our configure event with a new buffer.  Once the
 * client commits at the target size, scale → 1.0 naturally.
 *
 * Subsurfaces get the same scale factor (their positions are not
 * adjusted).  For most apps without complex relative-positioned
 * subsurfaces (browsers, terminals, games), this is visually
 * indistinguishable from a true uniform scale.
 */
static void
scene_buffer_scale_iter(struct wlr_scene_buffer *buf, int sx, int sy, void *data)
{
	double *scale = data;
	int w, h;

	(void)sx; (void)sy;
	if (!buf || !buf->buffer)
		return;

	w = (int)((double)buf->buffer->width * scale[0]);
	h = (int)((double)buf->buffer->height * scale[1]);
	if (w <= 0 || h <= 0)
		return;
	wlr_scene_buffer_set_dest_size(buf, w, h);
}

static void
scene_buffer_natural_iter(struct wlr_scene_buffer *buf, int sx, int sy, void *data)
{
	(void)sx; (void)sy; (void)data;
	if (!buf)
		return;
	wlr_scene_buffer_set_dest_size(buf, 0, 0);
}

void
client_scale_to_box(Client *c, int box_w, int box_h)
{
	struct wlr_box natural;
	double scale[2];

	if (!c || !c->scene_surface || !client_surface(c) ||
			!client_surface(c)->mapped)
		return;

	client_get_geometry(c, &natural);
	if (natural.width <= 0 || natural.height <= 0 || box_w <= 0 || box_h <= 0)
		return;

	scale[0] = (double)box_w / (double)natural.width;
	scale[1] = (double)box_h / (double)natural.height;

	wlr_scene_node_for_each_buffer(&c->scene_surface->node,
			scene_buffer_scale_iter, scale);
}

void
client_scale_reset(Client *c)
{
	if (!c || !c->scene_surface)
		return;
	wlr_scene_node_for_each_buffer(&c->scene_surface->node,
			scene_buffer_natural_iter, NULL);
}

/*
 * Niri-style spring animation primitives.
 *
 * Defaults match Niri's built-in animation spring values:
 *   horizontal-view-movement: damping 1.0, stiffness 800
 *   workspace-switch:         damping 1.0, stiffness 1000
 *   window-resize / movement: damping 1.0, stiffness 800
 *
 * Critical damping (ratio=1) gives a smooth, no-overshoot settle in
 * roughly ~250ms.  Semi-implicit Euler with sub-stepping keeps the
 * sim stable even at large dt (e.g. first frame after idle).
 */
#define ANIM_RATE_DEFAULT   40.0  /* legacy exp-decay, kept for compat with anim_tick callers */
#define ANIM_RATE_WS_SWITCH 60.0
#define ANIM_SETTLED_POS    0.5
#define ANIM_SETTLED_VEL    2.0

/* Horizontal scroll: same stiffness as workspace switch (1800) so
 * tile-select feels identical to ws-switch — user reported ws-switch
 * smooth but scroll slow at 1500; matching the curves removes that
 * perceived asymmetry. */
static const SpringParams SPRING_HORIZONTAL = { 1.0, 1.0, 1800.0 };
/* Workspace switch: 1800 → omega ≈ 42 → settle ≈ 120ms with a soft
 * critically-damped approach.  Slightly less stiff than 2500 →
 * gentler initial velocity → perceived as smoother while still
 * arriving fast.  Critical damping preserves "no overshoot" — no
 * oscillation past the target ws position. */
static const SpringParams SPRING_WS_SWITCH  = { 1.0, 1.0, 1800.0 };
static const SpringParams SPRING_WINDOW     = { 1.0, 1.0,  800.0 };
static const SpringParams SPRING_OPEN       = { 1.0, 0.9,  900.0 }; /* slight overshoot for life */
static const SpringParams SPRING_CLOSE      = { 1.0, 1.0,  900.0 };

int
anim_tick(double *current, double target, double rate, double dt)
{
	double diff;

	if (!current)
		return 0;

	diff = target - *current;
	if (fabs(diff) < ANIM_SETTLED_POS) {
		if (*current == target)
			return 0;
		*current = target;
		return 1;
	}

	*current += diff * (1.0 - exp(-rate * dt));
	return 1;
}

/*
 * Spring tick: update (pos, vel) toward target using a damped harmonic
 * oscillator.  Semi-implicit Euler with sub-stepping for stability.
 * Returns 1 if still moving, 0 if settled.
 */
int
spring_tick(double *pos, double *vel, double target, SpringParams sp, double dt)
{
	double omega2, damp_c, accel, sub_dt;
	int steps, i;

	if (!pos || !vel)
		return 0;
	if (sp.mass <= 0.0 || sp.stiffness <= 0.0)
		return 0;

	if (fabs(*pos - target) < ANIM_SETTLED_POS &&
			fabs(*vel) < ANIM_SETTLED_VEL) {
		if (*pos == target && *vel == 0.0)
			return 0;
		*pos = target;
		*vel = 0.0;
		return 1;
	}

	omega2 = sp.stiffness / sp.mass;
	damp_c = 2.0 * sp.damping * sqrt(sp.stiffness * sp.mass) / sp.mass;

	/* Sub-step: keep effective dt <= 4ms so omega*dt stays safely
	 * inside the stable region for semi-implicit Euler. */
	steps = (int)ceil(dt / 0.004);
	if (steps < 1) steps = 1;
	if (steps > 64) steps = 64;
	sub_dt = dt / (double)steps;

	for (i = 0; i < steps; i++) {
		accel = -omega2 * (*pos - target) - damp_c * (*vel);
		*vel += accel * sub_dt;
		*pos += *vel * sub_dt;
	}

	if (fabs(*pos - target) < ANIM_SETTLED_POS &&
			fabs(*vel) < ANIM_SETTLED_VEL) {
		*pos = target;
		*vel = 0.0;
	}
	return 1;
}

/*
 * Set a client's target geometry.
 *
 * Column clients have two distinct transition modes:
 *   1. Position-only change (camera scroll, ws Y switch): the layout
 *      drives c->geom every frame from scroll_x / ws_y_offset which
 *      themselves animate.  Snap c->geom to target — adding a second
 *      per-client anim layer would just lag.
 *   2. Size change (column-fullscreen toggle): the column's box
 *      width/height transitions.  Animate via the per-client anim
 *      tick, which also applies scene-buffer scaling each frame for
 *      smooth realtime content scaling.
 *
 * Floating / fullscreen / non-column clients always use the anim
 * path (they get step-change targets like fullscreen geom).
 *
 * Once an anim is in progress (anim_active == 1), subsequent
 * target updates from monitor_apply_positions just refresh
 * target_geom — they do NOT interrupt the running anim.
 */
void
client_set_target_geom(Client *c, struct wlr_box g)
{
	if (!c)
		return;

	c->target_geom = g;

	if (!client_surface(c) || !client_surface(c)->mapped) {
		c->geom = g;
		c->anim_active = 0;
		return;
	}

	if (c->geom.x == g.x && c->geom.y == g.y &&
			c->geom.width == g.width && c->geom.height == g.height) {
		c->anim_active = 0;
		return;
	}

	if (c->column) {
		int size_changed = (c->geom.width != g.width ||
				c->geom.height != g.height);
		if (size_changed) {
			/* Configure with the FINAL target size (col->target_width,
			 * col->target_height per-client share), not the lerped
			 * intermediate `g` — the client renders ONCE at the size
			 * it will settle at.  Re-sent whenever the final target
			 * moves mid-anim (interactive column drag-resize), so the
			 * client's content tracks the live edge instead of only
			 * updating after release. */
			int nc = c->column->n_clients;
			int gap = c->mon && c->mon->gaps ? (int)gappx : 0;
			int per_h_target;
			if (nc > 0) {
				/* Weighted height share — must mirror
				 * monitor_apply_positions so the configured
				 * final size matches the settle geometry. */
				int avail_t = c->column->target_height
						- gap * (nc - 1);
				double w = c->col_weight > 0.0
						? c->col_weight : 1.0;
				double sumw = 0.0;
				Client *cc;
				wl_list_for_each(cc, &c->column->clients,
						column_link)
					sumw += cc->col_weight > 0.0
							? cc->col_weight : 1.0;
				if (sumw <= 0.0)
					sumw = (double)nc;
				per_h_target = (int)((double)avail_t * w / sumw);
			} else {
				per_h_target = c->column->target_height;
			}
			int final_w = c->column->target_width  - 2 * c->bw;
			int final_h = per_h_target - 2 * c->bw;
			if (final_w < 1) final_w = 1;
			if (final_h < 1) final_h = 1;
			if (!c->anim_active) {
				struct wlr_box nat;
				c->geom_fx = (double)c->geom.x;
				c->geom_fy = (double)c->geom.y;
				c->geom_fw = (double)c->geom.width;
				c->geom_fh = (double)c->geom.height;
				c->geom_vx = c->geom_vy = c->geom_vw = c->geom_vh = 0.0;
				client_set_size(c, final_w, final_h);
				c->anim_final_w = final_w;
				c->anim_final_h = final_h;
				client_get_geometry(c, &nat);
				c->anim_start_nat_w = nat.width;
				c->anim_start_nat_h = nat.height;
			} else if (final_w != c->anim_final_w ||
					final_h != c->anim_final_h) {
				client_set_size(c, final_w, final_h);
				c->anim_final_w = final_w;
				c->anim_final_h = final_h;
			}
			c->anim_active = 1;
		} else {
			c->anim_active = 0;
			resize(c, g, 0);
		}
	} else {
		/* Floating / non-column: per-client anim path. */
		int size_changed = (c->geom.width != g.width ||
				c->geom.height != g.height);
		int final_w = g.width  - 2 * c->bw;
		int final_h = g.height - 2 * c->bw;
		if (final_w < 1) final_w = 1;
		if (final_h < 1) final_h = 1;
		if (!c->anim_active) {
			c->geom_fx = (double)c->geom.x;
			c->geom_fy = (double)c->geom.y;
			c->geom_fw = (double)c->geom.width;
			c->geom_fh = (double)c->geom.height;
			c->geom_vx = c->geom_vy = c->geom_vw = c->geom_vh = 0.0;
			if (size_changed) {
				/* Configure the final size NOW (fullscreen toggle,
				 * float-resize) — the client renders its new
				 * content during the anim, not after settle. */
				struct wlr_box nat;
				client_set_size(c, final_w, final_h);
				c->anim_final_w = final_w;
				c->anim_final_h = final_h;
				client_get_geometry(c, &nat);
				c->anim_start_nat_w = nat.width;
				c->anim_start_nat_h = nat.height;
			} else {
				c->anim_final_w = c->anim_final_h = 0;
			}
		} else if (size_changed && c->anim_final_w > 0 &&
				(final_w != c->anim_final_w ||
				 final_h != c->anim_final_h)) {
			client_set_size(c, final_w, final_h);
			c->anim_final_w = final_w;
			c->anim_final_h = final_h;
		}
		c->anim_active = 1;
	}
}

/*
 * Per-client geometry tick.  Advances c->geom toward c->target_geom
 * for any client with an active animation.  Called from
 * monitor_anim_tick once per frame.  Returns 1 if any client moved.
 */
/* Per-frame scene update during a column geom anim.  Writes:
 *   - scene tree position (anchor point shifts as left edge moves)
 *   - border rect size/position (border frames the animating box)
 *   - frozen_buffer dest_size (the snapshot scales to fill the
 *     lerped box) — this is what makes a Blender / heavy-app
 *     fullscreen toggle smooth: the cached single-texture snapshot
 *     stretches with the box, no black exposed area, and Blender
 *     doesn't have to repaint until the anim settles.  When no
 *     freeze is active (rare — only for the brief window before
 *     monitor_freeze_clients runs), we fall back to live surface
 *     animation. */
static void
client_anim_apply(Client *c, struct wlr_box g)
{
	int inner_w, inner_h;

	if (!c || !c->scene || !client_surface(c) || !client_surface(c)->mapped)
		return;
	c->geom = g;
	wlr_scene_node_set_position(&c->scene->node, g.x, g.y);
	wlr_scene_node_set_position(&c->scene_surface->node, c->bw, c->bw);
	if (c->border[0]) {
		wlr_scene_rect_set_size(c->border[0], g.width, c->bw);
		wlr_scene_rect_set_size(c->border[1], g.width, c->bw);
		wlr_scene_rect_set_size(c->border[2], c->bw, g.height - 2 * c->bw);
		wlr_scene_rect_set_size(c->border[3], c->bw, g.height - 2 * c->bw);
		wlr_scene_node_set_position(&c->border[1]->node, 0,
				g.height - c->bw);
		wlr_scene_node_set_position(&c->border[2]->node, 0, c->bw);
		wlr_scene_node_set_position(&c->border[3]->node,
				g.width - c->bw, c->bw);
	}

	/* Resize the frozen snapshot to match the lerped box.  This is
	 * the single biggest visual fix: instead of leaving a black
	 * exposed area where the surface natural size doesn't cover the
	 * box, we stretch the cached buffer to fill it. */
	inner_w = g.width  - 2 * c->bw;
	inner_h = g.height - 2 * c->bw;
	if (inner_w < 1) inner_w = 1;
	if (inner_h < 1) inner_h = 1;

	/* Early unfreeze: the moment the client commits a buffer at a NEW
	 * natural size (it was configured with the final size at anim
	 * start), swap the stretched stale snapshot for the live surface.
	 * Fresh content then shows mid-anim, scaled into the moving box,
	 * instead of popping in only after the anim settles. */
	if (c->frozen_buffer && c->anim_final_w > 0) {
		struct wlr_box nat;
		client_get_geometry(c, &nat);
		if (nat.width > 0 && nat.height > 0 &&
				(nat.width != c->anim_start_nat_w ||
				 nat.height != c->anim_start_nat_h))
			client_unfreeze(c);
	}

	if (c->frozen_buffer) {
		wlr_scene_buffer_set_dest_size(c->frozen_buffer,
				inner_w, inner_h);
	} else {
		/* Live surface during a size anim — keep it scaled to the
		 * lerped box every frame so content tracks the moving edge. */
		client_scale_to_box(c, inner_w, inner_h);
	}
}

static int
clients_anim_tick(Monitor *m, double dt)
{
	Client *c;
	int active = 0;

	wl_list_for_each(c, &clients, link) {
		struct wlr_box g;
		int moved = 0;

		if (c->mon != m || !c->anim_active)
			continue;
		if (!client_surface(c) || !client_surface(c)->mapped) {
			c->anim_active = 0;
			continue;
		}

		if (c->column) {
			/* Column clients: ALL FOUR axes are derived each frame
			 * from upstream springs (m->w, scroll_x_f, col->x_f,
			 * col->width_f).  Re-springing here would introduce a
			 * second-stage lag, breaking edge sync (the side
			 * facing the screen wobbles when target == current
			 * because the spring trails the source).  Snap to the
			 * live target — the visual lerp emerges from the
			 * upstream parameter springs which all use the same
			 * stiffness/damping, so they stay in lock-step. */
			c->geom_fx = (double)c->target_geom.x;
			c->geom_fy = (double)c->target_geom.y;
			c->geom_fw = (double)c->target_geom.width;
			c->geom_fh = (double)c->target_geom.height;
			c->geom_vx = c->geom_vy = c->geom_vw = c->geom_vh = 0.0;
			moved = (c->geom.x != c->target_geom.x ||
				c->geom.y != c->target_geom.y ||
				c->geom.width != c->target_geom.width ||
				c->geom.height != c->target_geom.height);
		} else {
			/* Floating / non-column: target is static during
			 * the anim, so per-axis spring on all 4 is correct
			 * (sides with target == current stay locked via
			 * spring_tick's early-out). */
			moved |= spring_tick(&c->geom_fx, &c->geom_vx,
					(double)c->target_geom.x,
					SPRING_WINDOW, dt);
			moved |= spring_tick(&c->geom_fy, &c->geom_vy,
					(double)c->target_geom.y,
					SPRING_WINDOW, dt);
			moved |= spring_tick(&c->geom_fw, &c->geom_vw,
					(double)c->target_geom.width,
					SPRING_WINDOW, dt);
			moved |= spring_tick(&c->geom_fh, &c->geom_vh,
					(double)c->target_geom.height,
					SPRING_WINDOW, dt);
		}

		if (moved) {
			g.x = (int)c->geom_fx;
			g.y = (int)c->geom_fy;
			g.width = (int)c->geom_fw;
			g.height = (int)c->geom_fh;
			client_anim_apply(c, g);
			active = 1;
		} else {
			resize(c, c->target_geom, 0);
			client_scale_reset(c);
			c->anim_active = 0;
		}
	}

	return active;
}

/*
 * Freeze: snapshot the current root buffer and disable scene_surface
 * so ONLY the snapshot renders during the anim.  This is critical for
 * surfaces with alpha (Alacritty, transparent terminals): if both
 * scene_surface and frozen_buffer rendered, the two transparent
 * layers would composite together → visible darkening during the
 * anim, "lightening" again on unfreeze.  Disabling scene_surface
 * keeps the visible result identical pre- and post-anim.
 *
 * Lock/unlock is handled internally by wlr_scene_buffer_create /
 * scene_node_destroy — no manual buffer_lock needed.
 */
static void
client_freeze(Client *c)
{
	struct wlr_surface *surface;

	if (!c || c->frozen_buffer || !c->scene)
		return;
	surface = client_surface(c);
	if (!surface || !surface->buffer || !surface->mapped)
		return;

	c->frozen_buffer = wlr_scene_buffer_create(c->scene,
			&surface->buffer->base);
	if (!c->frozen_buffer)
		return;

	wlr_scene_node_set_position(&c->frozen_buffer->node, c->bw, c->bw);
	wlr_scene_buffer_set_dest_size(c->frozen_buffer,
			c->geom.width - 2 * c->bw,
			c->geom.height - 2 * c->bw);

	if (c->scene_surface)
		wlr_scene_node_set_enabled(&c->scene_surface->node, 0);
}

void
client_unfreeze(Client *c)
{
	if (!c || !c->frozen_buffer)
		return;
	/* scene_node_destroy releases the wlr_buffer lock for us. */
	wlr_scene_node_destroy(&c->frozen_buffer->node);
	c->frozen_buffer = NULL;
	if (c->scene_surface)
		wlr_scene_node_set_enabled(&c->scene_surface->node, 1);
}

/* Two-tier freeze: X11 frozen on any anim (heavy, no subsurfaces).
 * Wayland frozen only on size anim — root buffer snapshot drops
 * subsurfaces / popups, so freezing during pure pos anims (ws switch,
 * tile select) makes Firefox / Chrome lose their shape and the CSD
 * edge appear to leak into adjacent workspaces. */
static void
monitor_freeze_clients(Monitor *m, int include_x11, int include_wayland)
{
	Client *c;
	wl_list_for_each(c, &clients, link) {
		int is_x11;
		if (c->mon != m || !client_surface(c) ||
				!client_surface(c)->mapped ||
				c->frozen_buffer)
			continue;
		/* Open anim needs the LIVE surface so the opacity tick is
		 * visible — freezing would lock the snapshot at the static
		 * pre-anim state and the fade-in wouldn't render. */
		if (c->open_anim_active)
			continue;
		/* Fullscreen clients: never freeze.  The root-buffer snapshot
		 * drops subsurfaces (browser video lives in one), so a frozen
		 * fullscreen browser renders as a static black page while its
		 * scene_surface is disabled — black screen with live cursor. */
		if (c->isfullscreen)
			continue;
#ifdef XWAYLAND
		is_x11 = client_is_x11(c);
#else
		is_x11 = 0;
#endif
		if (is_x11 ? !include_x11 : !include_wayland)
			continue;
		client_freeze(c);
	}
}

static void
monitor_unfreeze_clients(Monitor *m, int include_x11, int include_wayland)
{
	Client *c;
	wl_list_for_each(c, &clients, link) {
		int is_x11;
		if (c->mon != m || !c->frozen_buffer)
			continue;
#ifdef XWAYLAND
		is_x11 = client_is_x11(c);
#else
		is_x11 = 0;
#endif
		if (is_x11 ? !include_x11 : !include_wayland)
			continue;
		int inner_w = c->geom.width  - 2 * c->bw;
		int inner_h = c->geom.height - 2 * c->bw;
		if (inner_w < 1) inner_w = 1;
		if (inner_h < 1) inner_h = 1;
		wlr_scene_buffer_set_dest_size(c->frozen_buffer,
				inner_w, inner_h);
		client_unfreeze(c);
		/* Scale the live surface to fill the box until the client
		 * commits a buffer at the new natural size.  Without this,
		 * an unfreeze right after a size anim can flash the OLD
		 * natural-sized surface in the new larger box (= black
		 * exposed area on the growing edge for heavy/slow clients
		 * like Blender). */
		client_scale_to_box(c, inner_w, inner_h);
	}
}

/*
 * Per-monitor animation tick.  Advances:
 *   - active workspace's scroll_x → target_scroll_x (camera follow)
 *   - monitor's ws_y_offset → 0    (vertical workspace switch decay)
 *   - per-client geom            (fullscreen, spawn slide, swap)
 *
 * Also drives freeze/unfreeze of all clients on this monitor at the
 * boundaries of an animation, so movement is glass-smooth even when
 * the underlying client is heavy or stalled.
 */
int
monitor_anim_tick(Monitor *m, double dt)
{
	int active = 0;
	int size_anim = 0;            /* freeze only when REAL size change in flight */
	Workspace *ws;
	int close_still = 0;
	int vertical_anim;

	if (!m)
		return 0;

	/* ── Step 1: tick all parameter springs (scroll, ws_y, col->x). ─
	 *   These feed monitor_apply_positions to recompute target_geom
	 *   for each client BEFORE the per-client clients_anim_tick uses
	 *   those targets.  Without this ordering the client spring snaps
	 *   x/y to a stale (one-frame-old) target while width animates,
	 *   so an "anchored" edge against the screen wobbles instead of
	 *   staying locked. */

	if (m->active_ws) {
		ws = m->active_ws;
		if (ws->scroll_x_f == 0.0 && ws->scroll_x_vel == 0.0 &&
				ws->scroll_x != 0)
			ws->scroll_x_f = (double)ws->scroll_x;
		if (spring_tick(&ws->scroll_x_f, &ws->scroll_x_vel,
				(double)ws->target_scroll_x,
				SPRING_HORIZONTAL, dt))
			active = 1;
		ws->scroll_x = (int)ws->scroll_x_f;
	}

	if (spring_tick(&m->ws_y_offset, &m->ws_y_vel, 0.0,
			SPRING_WS_SWITCH, dt))
		active = 1;

	/* Tile-area spring (m->w).  When waybar (un)mounts or changes
	 * its exclusive zone, m->w_target shifts but m->w lerps —
	 * tile edges facing the change slide, opposite edges stay
	 * locked because the corresponding (y, height) springs use
	 * IDENTICAL parameters so their sum stays constant. */
	if (m->w_initialized) {
		int moved_pos = 0, moved_size = 0;
		moved_pos  |= spring_tick(&m->w_x_f, &m->w_x_vel,
				(double)m->w_target.x, SPRING_WINDOW, dt);
		moved_pos  |= spring_tick(&m->w_y_f, &m->w_y_vel,
				(double)m->w_target.y, SPRING_WINDOW, dt);
		moved_size |= spring_tick(&m->w_w_f, &m->w_w_vel,
				(double)m->w_target.width, SPRING_WINDOW, dt);
		moved_size |= spring_tick(&m->w_h_f, &m->w_h_vel,
				(double)m->w_target.height, SPRING_WINDOW, dt);
		if (moved_pos || moved_size) {
			active = 1;
			/* Derive width/height from the rounded FAR edge, not
			 * from independent truncation: y and height springs are
			 * symmetric so y_f + h_f is constant during a statusbar
			 * toggle, but (int)y_f + (int)h_f jitters ±1px — the
			 * bottom edge of every tile visibly wobbles. */
			m->w.x = (int)m->w_x_f;
			m->w.y = (int)m->w_y_f;
			m->w.width = (int)(m->w_x_f + m->w_w_f) - m->w.x;
			m->w.height = (int)(m->w_y_f + m->w_h_f) - m->w.y;
		}
		if (moved_size)
			size_anim = 1;
	}

	vertical_anim = (fabs(m->ws_y_offset) > 0.5 ||
			fabs(m->ws_y_vel) > 0.5);
	{
		Workspace *wsi;
		Column *col;
		wl_list_for_each(wsi, &m->workspaces, link) {
			if (!vertical_anim && wsi != m->active_ws)
				continue;
			wl_list_for_each(col, &wsi->columns, link) {
				if (spring_tick(&col->x_f, &col->x_vel,
						(double)col->target_x,
						SPRING_WINDOW, dt))
					active = 1;
				col->x = (int)col->x_f;
				if (spring_tick(&col->width_f,
						&col->width_vel,
						(double)col->target_width,
						SPRING_WINDOW, dt)) {
					active = 1;
					size_anim = 1;
				}
				col->width = (int)col->width_f;
			}
		}
	}

	/* ── Step 2: recompute target_geom only if a parameter spring
	 *   moved this frame (otherwise target_geom from the last
	 *   arrange() call is still authoritative — saves a full
	 *   per-client walk on idle frames). */
	if (active)
		monitor_apply_positions(m);

	/* ── Step 3: per-client spring (size anim only) reads the fresh
	 *   target_geom written by step 2. */
	if (clients_anim_tick(m, dt)) {
		active = 1;
		size_anim = 1;
	}

	/* Close anim tick — runs independent of clients list. */
	closing_anims_tick(m, dt, &close_still);
	if (close_still)
		active = 1;

	/* Open anim tick — per-client scale + fade. */
	{
		Client *c;
		wl_list_for_each(c, &clients, link) {
			if (c->mon != m || !c->open_anim_active)
				continue;
			if (spring_tick(&c->open_progress,
					&c->open_progress_vel, 1.0,
					SPRING_OPEN, dt)) {
				client_apply_open_anim(c);
				active = 1;
			} else {
				c->open_progress = 1.0;
				c->open_anim_active = 0;
				client_apply_open_anim(c);
			}
		}
	}

	/* Two-tier freeze:
	 *   X11 → freeze on ANY anim.  X11 doesn't use Wayland
	 *     subsurfaces, so root-buffer snapshot is complete.
	 *   Wayland → freeze ONLY on size anim.  Browsers (Firefox,
	 *     Chrome) render content in subsurfaces; root-buffer
	 *     snapshot drops them and the visible window becomes the
	 *     bare CSD frame, leaking into adjacent workspaces during
	 *     a ws-switch slide.  Pure pos anims keep the live surface.
	 */
	if (active && !m->anim_was_active)
		monitor_freeze_clients(m, /*x11=*/1, /*wl=*/0);
	if (size_anim && !m->size_anim_was_active)
		monitor_freeze_clients(m, /*x11=*/0, /*wl=*/1);
	if (!size_anim && m->size_anim_was_active)
		monitor_unfreeze_clients(m, /*x11=*/0, /*wl=*/1);
	if (!active && m->anim_was_active)
		monitor_unfreeze_clients(m, /*x11=*/1, /*wl=*/1);
	m->anim_was_active = active;
	m->size_anim_was_active = size_anim;
	return active;
}

/* ── Niri-style open anim ────────────────────────────────────────────
 * Per-buffer scale + opacity applied to live surface.  Animates
 * scale 0.5→1.0 and alpha 0→1 over ~250ms.  Center pivot so the
 * window grows from its geometric center, not the top-left.
 */
/* Niri-style open: opacity fade only.  Scaling subsurfaces via
 * per-buffer dest_size produces artifacts (subsurfaces stay anchored
 * at their natural positions while their content shrinks) — drop
 * the scale path entirely and rely on opacity.  Visually identical
 * to Niri for the common case (no per-app scale on open). */
static void
scene_buffer_opacity_iter(struct wlr_scene_buffer *buf, int sx, int sy, void *data)
{
	double *alpha = data;
	(void)sx; (void)sy;
	if (!buf)
		return;
	wlr_scene_buffer_set_opacity(buf, (float)*alpha);
}

void
client_apply_open_anim(Client *c)
{
	double alpha;

	if (!c || !c->scene_surface || !client_surface(c) ||
			!client_surface(c)->mapped)
		return;

	if (c->open_anim_active) {
		alpha = c->open_progress;
		if (alpha < 0.0) alpha = 0.0;
		if (alpha > 1.0) alpha = 1.0;
	} else {
		alpha = 1.0;
	}

	wlr_scene_node_for_each_buffer(&c->scene_surface->node,
			scene_buffer_opacity_iter, &alpha);
}

void
client_start_open_anim(Client *c)
{
	if (!c)
		return;
	c->open_progress = 0.0;
	c->open_progress_vel = 0.0;
	c->open_anim_active = 1;
	client_apply_open_anim(c);
}

/* ── Niri-style close anim ───────────────────────────────────────────
 * On unmap, snapshot the client's last buffer to an independent
 * scene tree.  Animate scale 1.0→0.5 + opacity 1→0 over ~200ms,
 * then free.  Survives the underlying Client destruction.
 */
void
anim_spawn_close(Monitor *m, struct wlr_buffer *buffer, struct wlr_box geom)
{
	ClosingAnim *a;
	struct wlr_scene_tree *parent;

	if (!buffer || geom.width <= 0 || geom.height <= 0)
		return;
	parent = layers[LyrFloat];
	if (!parent)
		return;

	a = ecalloc(1, sizeof(*a));
	if (!a)
		return;
	a->mon = m;
	a->geom = geom;
	a->natural_w = buffer->width;
	a->natural_h = buffer->height;
	a->progress = 1.0;
	a->vel = 0.0;

	a->tree = wlr_scene_tree_create(parent);
	if (!a->tree) {
		free(a);
		return;
	}
	a->buffer = wlr_scene_buffer_create(a->tree, buffer);
	if (!a->buffer) {
		wlr_scene_node_destroy(&a->tree->node);
		free(a);
		return;
	}
	wlr_scene_node_set_position(&a->tree->node, geom.x, geom.y);
	wlr_scene_buffer_set_dest_size(a->buffer, geom.width, geom.height);
	wlr_scene_buffer_set_opacity(a->buffer, 1.0f);

	wl_list_insert(&closing_anims, &a->link);

	/* Make sure the next vblank fires the anim tick. */
	if (m && m->wlr_output && !m->frame_scheduled) {
		wlr_output_schedule_frame(m->wlr_output);
		m->frame_scheduled = 1;
	}
}

void
closing_anims_tick(Monitor *m, double dt, int *still)
{
	ClosingAnim *a, *tmp;

	if (still) *still = 0;
	if (!m)
		return;

	wl_list_for_each_safe(a, tmp, &closing_anims, link) {
		if (a->mon != m)
			continue;
		int moving = spring_tick(&a->progress, &a->vel, 0.0,
				SPRING_CLOSE, dt);
		if (a->progress <= 0.02 && fabs(a->vel) < 1.0) {
			wl_list_remove(&a->link);
			if (a->tree)
				wlr_scene_node_destroy(&a->tree->node);
			free(a);
			continue;
		}
		/* Opacity-only close fade: no scale → no subsurface
		 * artifacts.  Position unchanged from snapshot geom. */
		wlr_scene_buffer_set_opacity(a->buffer,
				(float)a->progress);
		if (still && moving)
			*still = 1;
	}
}
