#include "nixlytile.h"
#include "client.h"
#include "diag.h"

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
struct scale_ctx {
	double scale[2];
	struct wlr_surface *root;   /* the client's toplevel surface */
	int box_w, box_h;           /* target inner box */
};

static void
scene_buffer_scale_iter(struct wlr_scene_buffer *buf, int sx, int sy, void *data)
{
	struct scale_ctx *ctx = data;
	struct wlr_scene_surface *ss;
	int w, h;

	(void)sx; (void)sy;
	if (!buf || !buf->buffer)
		return;

	ss = wlr_scene_surface_try_from_buffer(buf);
	if (ss && ss->surface == ctx->root) {
		/* Root surface: the window-geometry clip has already trimmed
		 * the CSD margin off the buffer, so the tile's inner box IS
		 * its dest size.  Deriving it from raw buffer pixels instead
		 * overshoots by exactly that margin — Chrome's 20px shadow
		 * made its content render 20px past the tile border on every
		 * scaled frame. */
		w = ctx->box_w;
		h = ctx->box_h;
	} else if (ss && ss->surface) {
		/* Subsurface: scale its logical (viewport-applied) size, not
		 * the buffer pixels, which differ under buffer_scale > 1. */
		w = (int)((double)ss->surface->current.width * ctx->scale[0]);
		h = (int)((double)ss->surface->current.height * ctx->scale[1]);
	} else {
		w = (int)((double)buf->buffer->width * ctx->scale[0]);
		h = (int)((double)buf->buffer->height * ctx->scale[1]);
	}
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
	int nat_w, nat_h;
	struct scale_ctx ctx;

	if (!c || !c->scene_surface || !client_surface(c) ||
			!client_surface(c)->mapped)
		return;

	/* A cropped tile (straddling the usable-area edge) is owned by
	 * client_clip_to_usable: the subsurface-tree clip has already set
	 * src/dest for the visible slice.  Raw dest_size scaling here would
	 * stretch that cropped source back to full box size — content
	 * bleeding over the statusbar / gap margins (worst with slow-acking
	 * browsers).  Skip; content re-scales once fully inside. */
	if (c->area_clipped)
		return;

	client_get_committed_size(c, &nat_w, &nat_h);
	if (nat_w <= 0 || nat_h <= 0 || box_w <= 0 || box_h <= 0)
		return;

	ctx.scale[0] = (double)box_w / (double)nat_w;
	ctx.scale[1] = (double)box_h / (double)nat_h;
	ctx.root = client_surface(c);
	ctx.box_w = box_w;
	ctx.box_h = box_h;

	wlr_scene_node_for_each_buffer(&c->scene_surface->node,
			scene_buffer_scale_iter, &ctx);
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
/* Column x/width MUST match the camera spring (SPRING_HORIZONTAL):
 * closing/moving/resizing a column animates col->x together with
 * scroll_x, and mismatched stiffness makes the reflowing columns
 * visibly trail the camera — tiles drift out of lock-step. */
static const SpringParams SPRING_COLUMN     = { 1.0, 1.0, 1800.0 };
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
static int live_resize_active(void);

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

	/* First placement (never been through resize(): last_size_w only
	 * gets set there): the scene node still sits at the (0,0) map
	 * default and c->geom holds the client's natural/X11-root coords —
	 * both usually on ANOTHER monitor.  Springing from there animates
	 * the window across screens before it lands.  Snap straight to the
	 * target instead: resize() writes the scene position synchronously,
	 * inside this arrange pass, before any output can render a frame. */
	if (c->last_size_w == 0 && c->last_size_h == 0) {
		c->anim_active = 0;
		resize(c, g, 0);
		return;
	}

	if (c->geom.x == g.x && c->geom.y == g.y &&
			c->geom.width == g.width && c->geom.height == g.height) {
		/* Target equals current geometry.  If an anim is in flight this
		 * is a mid-anim arrange() (springs un-ticked since last frame,
		 * so g == c->geom for every animating client) — do NOT cancel:
		 * clearing anim_active here abandons the settle (scale_reset +
		 * final resize) and orphans the final-size configure sent at
		 * anim start, leaving the tile cropped/stretched at the wrong
		 * size until layout numbers change again.  Keep the anim alive;
		 * clients_anim_tick either continues it or performs the full
		 * settle on the next frame (arrange scheduled one). */
		return;
	}

	if (c->column) {
		int size_changed = (c->geom.width != g.width ||
				c->geom.height != g.height);
		if (size_changed) {
			/* The final size (col->target_width, col->target_height
			 * per-client share) is configured at SETTLE — see the
			 * resize() in clients_anim_tick — not here.  Configuring
			 * it up front makes a fast client (Chrome acks in ~10ms)
			 * commit its final-size buffer while the box is still near
			 * its old size; client_scale_to_box then stretches that
			 * buffer up to the lerped box (measured 2.17×) and the
			 * content visibly zooms back down over the rest of the
			 * anim.  Withholding it keeps the client's own buffer as
			 * the natural size, so the scale walks monotonically
			 * 1.0 → final and the content simply follows the box.
			 *
			 * A live drag is the exception: the pointer owns the size
			 * there, and the client must re-render at the live edge
			 * instead of only after release. */
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
				c->geom_fx = (double)c->geom.x;
				c->geom_fy = (double)c->geom.y;
				c->geom_fw = (double)c->geom.width;
				c->geom_fh = (double)c->geom.height;
				c->geom_vx = c->geom_vy = c->geom_vw = c->geom_vh = 0.0;
				if (live_resize_active())
					client_request_size(c, final_w, final_h);
				c->anim_final_w = final_w;
				c->anim_final_h = final_h;
				client_get_committed_size(c,
						&c->anim_start_nat_w,
						&c->anim_start_nat_h);
			} else if (final_w != c->anim_final_w ||
					final_h != c->anim_final_h) {
				if (live_resize_active())
					client_request_size(c, final_w, final_h);
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
				/* Same as the column path above: the final size
				 * (fullscreen toggle, float-resize) is configured
				 * at settle, so the client's committed buffer stays
				 * the natural size the anim scales FROM. */
				if (live_resize_active())
					client_request_size(c, final_w, final_h);
				c->anim_final_w = final_w;
				c->anim_final_h = final_h;
				client_get_committed_size(c,
						&c->anim_start_nat_w,
						&c->anim_start_nat_h);
			} else {
				c->anim_final_w = c->anim_final_h = 0;
			}
		} else if (size_changed && c->anim_final_w > 0 &&
				(final_w != c->anim_final_w ||
				 final_h != c->anim_final_h)) {
			if (live_resize_active())
				client_request_size(c, final_w, final_h);
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
/* True while the user is dragging a tile edge or column border.
 *
 * Interactive resize must show REAL content, not a stretched snapshot: the
 * pointer sets the size directly, so there is no animation to cover up, and
 * scaling the old buffer is exactly the "everything stretches until you let
 * go, then reflows" effect. The client is reconfigured on every motion
 * event, so it reflows as the edge moves. */
static int
live_resize_active(void)
{
	return cursor_mode == CurResize || cursor_mode == CurColResize;
}

/* Surface commit while a geometry anim is in flight.  wlroots' own
 * commit handler (surface_reconfigure) has just reset every buffer's
 * dest_size to the surface's NATURAL size — which is the FINAL size we
 * configured at anim start.  Left uncorrected, the surface renders past
 * the lerped box until the next anim tick: the "locked" edge of the
 * tile visibly pops outward, and near a monitor edge the overshoot
 * spills onto the neighbouring output (another output's rendermon can
 * fire before our next tick re-scales).  Re-pin clip + scale now.
 * Registered in mapnotify AFTER the scene surface is created so it runs
 * after wlroots' handler. */
void
animcommitnotify(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, anim_commit);
	int iw, ih;

	(void)data;
	launchfx_note_commit(c);
	if (!c->scene_surface || !c->mon)
		return;
#ifdef XWAYLAND
	if (!c->anim_active && client_is_x11(c) && c->column) {
		/* X11 has no xdg commitnotify doing post-settle work — this
		 * hook is its only commit path.  Re-pin the clip against the
		 * fresh buffer, and self-heal a size desync: X11 has no ack
		 * chain, so a configure lost to an aborted anim (or a client
		 * that reasserted its own size) leaves the committed size
		 * permanently different from the tile box.  request_size's
		 * dedup makes this a no-op whenever last_configured already
		 * matches the box. */
		int nw, nh;
		iw = c->geom.width  - 2 * (int)c->bw;
		ih = c->geom.height - 2 * (int)c->bw;
		client_clip_to_usable(c);
		client_get_committed_size(c, &nw, &nh);
		if (iw > 0 && ih > 0 && nw > 0 && nh > 0 &&
				(nw != iw || nh != ih))
			client_request_size(c, iw, ih);
		return;
	}
#endif
	if (!c->anim_active)
		return;
	/* Live drag shows real content at real size — natural is correct.
	 * Still re-pin the clip: this commit may carry a buffer larger than
	 * the tile box, and a neighbouring output's rendermon can fire
	 * before the next anim tick re-clips it. */
	if (cursor_mode == CurResize || cursor_mode == CurColResize) {
		client_clip_to_usable(c);
		return;
	}
	if (c->frozen_buffer)
		return;
	iw = c->geom.width - 2 * c->bw;
	ih = c->geom.height - 2 * c->bw;
	if (iw < 1) iw = 1;
	if (ih < 1) ih = 1;
	/* Clip first: if the clip box changed, set_clip reconfigures
	 * dest_size again — scaling after keeps the scale authoritative. */
	client_clip_to_usable(c);
	client_scale_to_box(c, iw, ih);
}

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
		client_set_border_size(c, g.width, g.height);
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

	/* Live drag: no snapshot. The surface renders at the size it was just
	 * configured with, so text reflows and layouts recompute while the
	 * edge moves; client_clip_to_usable scales the committed buffer into
	 * the box for the frames the client is still behind (scale 1.0, i.e.
	 * a no-op, as soon as it catches up). */
	if (live_resize_active()) {
		if (c->frozen_buffer)
			client_unfreeze(c);
		client_clip_to_usable(c);
		return;
	}

	/* Early unfreeze: the moment the client commits a buffer at a NEW
	 * natural size (it was configured with the final size at anim
	 * start), swap the stretched stale snapshot for the live surface.
	 * Fresh content then shows mid-anim, scaled into the moving box,
	 * instead of popping in only after the anim settles. */
	if (c->frozen_buffer && c->anim_final_w > 0) {
		int nat_w, nat_h;
		client_get_committed_size(c, &nat_w, &nat_h);
		if (nat_w > 0 && nat_h > 0 &&
				(nat_w != c->anim_start_nat_w ||
				 nat_h != c->anim_start_nat_h))
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

	/* Crop to the usable tile area so an animating tile never overshoots
	 * into the gap margin (covers surface, borders, and this snapshot). */
	client_clip_to_usable(c);
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
			/* Round the far edge, not the size: a locked
			 * bottom/right edge (x and width springs cancelling)
			 * stays put instead of jittering ±1px from two
			 * independent truncations. */
			g.x = (int)c->geom_fx;
			g.y = (int)c->geom_fy;
			g.width = (int)(c->geom_fx + c->geom_fw) - g.x;
			g.height = (int)(c->geom_fy + c->geom_fh) - g.y;
			client_anim_apply(c, g);
			active = 1;
		} else {
			/* Settle: clear anim_active FIRST so the clip inside
			 * resize() runs in box-clip mode — a client that hasn't
			 * committed the final size yet must be cropped to its
			 * box, not left bleeding over the neighbour until the
			 * commit lands.  scale_reset before resize() so the
			 * clip's dest sizing stays authoritative. */
			c->anim_active = 0;
			/* Mid-drag settle (pointer held still for a frame):
			 * keep the live-drag scale — resetting it drops the
			 * fit until the client's next commit, and resize()'s
			 * no-change fast-path may not re-apply it. */
			if (!live_resize_active())
				client_scale_reset(c);
			resize(c, c->target_geom, 0);
#ifdef XWAYLAND
			/* client_request_size's size-only dedup drops the
			 * configure when just the POSITION changed (fullscreen
			 * exit returns the tile to its slot at unchanged size).
			 * The X11 client then keeps stale root coords and
			 * misplaces menus/tooltips.  xsurface->x/y mirror the
			 * last configure actually sent — flush once at settle
			 * if they disagree with where the window ended up. */
			if (client_is_x11(c) &&
					(c->surface.xwayland->x !=
						c->geom.x + (int)c->bw ||
					 c->surface.xwayland->y !=
						c->geom.y + (int)c->bw))
				client_set_size(c,
					c->geom.width - 2 * (int)c->bw,
					c->geom.height - 2 * (int)c->bw);
#endif
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
	if (!surface || !surface->mapped)
		return;
	if (!surface->buffer) {
		/* Ingen buffer å snapshotte: klienten har ikke levert innhold
		 * (typisk en X11-klient som har stått skjult på en inaktiv
		 * workspace). Live-flaten står igjen uten innhold — tilen
		 * rendres tom til klienten tegner igjen. */
		diag_logf("TILE", "FREEZE-SKIP appid='%s' %dx%d (no buffer — tile renders empty)",
			client_get_appid(c) ? client_get_appid(c) : "(null)",
			c->geom.width, c->geom.height);
		return;
	}

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

	/* Crop the fresh snapshot NOW.  Freeze runs at the END of the anim
	 * tick, after this frame's clip pass — a tile straddling (or scrolled
	 * past) the monitor edge would otherwise carry a full-size unclipped
	 * snapshot until the next tick, and the neighbouring output's
	 * rendermon can fire in that window and paint it across the edge
	 * (Steam/X11: frozen on every scroll step → constant flicker on the
	 * neighbour screen). */
	client_clip_to_usable(c);

	/* A frozen tile shows a static snapshot with its live surface disabled.
	 * Normal during an anim (paired with UNFREEZE); a FREEZE with no matching
	 * UNFREEZE = a tile stuck frozen. */
	diag_logf("TILE", "FREEZE appid='%s' %dx%d (live surface disabled, snapshot shown)",
		client_get_appid(c) ? client_get_appid(c) : "(null)",
		c->geom.width, c->geom.height);
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

	diag_logf("TILE", "UNFREEZE appid='%s' (live surface re-enabled)",
		client_get_appid(c) ? client_get_appid(c) : "(null)");
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

	/* Never during a drag — the point of a live resize is that the client
	 * repaints as the edge moves, and a snapshot would hide exactly that. */
	if (live_resize_active())
		return;

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
	int camera_anim = 0;          /* scroll_x / ws_y spring moving this frame */
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
				SPRING_HORIZONTAL, dt)) {
			active = 1;
			camera_anim = 1;
		}
		ws->scroll_x = (int)ws->scroll_x_f;
	}

	if (spring_tick(&m->ws_y_offset, &m->ws_y_vel, 0.0,
			SPRING_WS_SWITCH, dt)) {
		active = 1;
		camera_anim = 1;
	}

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
			/* The bar rides the same spring — see
			 * statusbar_anim_sync. */
			statusbar_anim_sync(m);
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
						SPRING_COLUMN, dt))
					active = 1;
				col->x = (int)col->x_f;
				if (spring_tick(&col->width_f,
						&col->width_vel,
						(double)col->target_width,
						SPRING_COLUMN, dt)) {
					active = 1;
					size_anim = 1;
				}
				/* Far-edge rounding (same as m->w above): x and
				 * width springs cancel exactly on a locked right
				 * edge, but (int)x_f + (int)width_f jitters ±1px. */
				col->width = (int)(col->x_f + col->width_f)
						- col->x;
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

	/* Varsel-slide i høyre marg. Egen liste: override-redirect-klienter
	 * står ikke i `clients`, så clients_anim_tick ser dem aldri. */
	{
		int notif_still = 0;
		notify_tick(m, dt, &notif_still);
		if (notif_still)
			active = 1;
		osd_tick(m, dt, &notif_still);
		if (notif_still)
			active = 1;
	}

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

	/* Freeze policy — live content is the default, snapshot the exception:
	 *
	 *   SIZE anim (tile resize, Mod+F fullscreen) → NEVER freeze, any
	 *     client type.  client_anim_apply scales the LIVE surface into the
	 *     lerped box every frame (client_scale_to_box, which walks every
	 *     scene buffer so subsurfaces — browser/Electron video — scale
	 *     too).  Combined with the final-size configure sent at anim start
	 *     (client_set_target_geom), content stays live and continuous the
	 *     whole slide and snaps crisp the instant the client commits the
	 *     new size — for X11, Electron and native Wayland alike.  No
	 *     black growing edge: the stale buffer is stretched to fill until
	 *     the fresh one lands.
	 *
	 *   PURE POSITION anim (ws-switch slide) → freeze X11 only.  X11 has
	 *     no subsurfaces so its root snapshot is complete, and it avoids
	 *     X11 movement tearing.  Wayland is never frozen (snapshot drops
	 *     its subsurfaces → CSD frame leaks into the adjacent workspace);
	 *     its live surface just translates, which needs no content update
	 *     to look right.
	 */
	{
		int pos_only = active && !size_anim;
		if (pos_only && !m->pos_anim_was_active)
			monitor_freeze_clients(m, /*x11=*/1, /*wl=*/0);
		if (!pos_only && m->pos_anim_was_active)
			monitor_unfreeze_clients(m, /*x11=*/1, /*wl=*/0);
		m->pos_anim_was_active = pos_only;
	}
	/* Camera slide (tile-select scroll / ws switch) with no size change:
	 * rendermon withholds frame_done from clients this frame so heavy
	 * tiles (Blender playing an animation) pause their render loop for
	 * the ~150ms slide instead of racing the compositor for GPU time.
	 * A slide only translates existing buffers — no client repaint can
	 * improve it.  Size anims are excluded: there the client MUST
	 * repaint to converge on its new size. */
	/* A live drag also excludes the throttle: the dragged client must
	 * repaint at the sizes we are configuring, and withholding its frame
	 * callbacks stalls it behind the pointer for the whole drag. */
	m->camera_anim_active = camera_anim && !size_anim &&
			!live_resize_active();
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
	/* Crop to the monitor's usable area, like client_clip_to_usable does
	 * for the live tile: a tile closing while partially scrolled past
	 * the monitor edge otherwise paints its hidden part across the
	 * neighbouring output for the duration of the fade. */
	if (m) {
		struct wlr_box vis;
		if (!wlr_box_intersection(&vis, &geom, &m->w)) {
			wlr_scene_node_destroy(&a->tree->node);
			free(a);
			return;
		}
		if (vis.x != geom.x || vis.y != geom.y ||
				vis.width != geom.width ||
				vis.height != geom.height) {
			struct wlr_fbox src = {
				.x = (double)(vis.x - geom.x) * buffer->width
						/ geom.width,
				.y = (double)(vis.y - geom.y) * buffer->height
						/ geom.height,
				.width  = (double)vis.width * buffer->width
						/ geom.width,
				.height = (double)vis.height * buffer->height
						/ geom.height,
			};
			wlr_scene_buffer_set_source_box(a->buffer, &src);
		}
		wlr_scene_node_set_position(&a->tree->node, vis.x, vis.y);
		wlr_scene_buffer_set_dest_size(a->buffer, vis.width, vis.height);
	} else {
		wlr_scene_node_set_position(&a->tree->node, geom.x, geom.y);
		wlr_scene_buffer_set_dest_size(a->buffer, geom.width, geom.height);
	}
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
