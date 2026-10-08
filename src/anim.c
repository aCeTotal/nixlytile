#include "nixlytile.h"
#include "client.h"
#include "diag.h"
#include "overview.h"

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
		/* buf->buffer is NULL once the texture is uploaded */
		if (!buf->buffer)
			return;
		w = (int)((double)buf->buffer->width * ctx->scale[0]);
		h = (int)((double)buf->buffer->height * ctx->scale[1]);
	}
	if (w <= 0 || h <= 0)
		return;
	wlr_scene_buffer_set_dest_size(buf, w, h);
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
/* Open fade: stiffness 900 settled in ~150ms, which is 150ms of a window
 * that is ALREADY rendered sitting there half-transparent — the whole
 * compositor-side share of "nothing opens instantly".  4900 (ω=70) keeps
 * the fade visible but puts it under human reaction time. */
static const SpringParams SPRING_OPEN       = { 1.0, 0.9, 4900.0 }; /* slight overshoot for life */
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

/* Floating geometry spring, content-synced. */
void
client_set_target_geom(Client *c, struct wlr_box g)
{
	struct wlr_box from;

	if (!c)
		return;

	c->target_geom = g;

	if (!client_surface(c) || !client_surface(c)->mapped) {
		c->geom = g;
		c->anim_active = 0;
		return;
	}

	/* First placement snaps: no cross-screen slide. */
	if (c->last_size_w == 0 && c->last_size_h == 0) {
		c->anim_active = 0;
		resize(c, g, 0);
		return;
	}

	/* Mid-anim arrange keeps the anim alive. */
	from = c->float_want_set ? c->float_want : c->geom;
	if (wlr_box_equal(&from, &g))
		return;

	if (!c->anim_active) {
		c->geom_fx = (double)from.x;
		c->geom_fy = (double)from.y;
		c->geom_fw = (double)from.width;
		c->geom_fh = (double)from.height;
		c->geom_vx = c->geom_vy = c->geom_vw = c->geom_vh = 0.0;
	}
	c->anim_active = 1;
}

/* Pointer drags a tile edge. */
static int
live_resize_active(void)
{
	return cursor_mode == CurResize || cursor_mode == CurColResize;
}

/* Runs after the scene saw the commit. */
void
animcommitnotify(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, anim_commit);

	(void)data;
	launchfx_note_commit(c);
	/* Wake the size-convergence watchdog only when the COMMITTED size
	 * changed — kicking on every commit would keep its per-frame client
	 * walk alive for the whole lifetime of any animating/video client. */
	{
		int cw = 0, ch = 0;
		client_get_committed_size(c, &cw, &ch);
		if (cw != c->conv_seen_w || ch != c->conv_seen_h) {
			c->conv_seen_w = cw;
			c->conv_seen_h = ch;
			converge_kick(c);
		}
	}
	/* X11 clients have no xdg commitnotify, so fullscreen frame-rate
	 * detection never got a single sample from them — video pacing and
	 * the idle-inhibit "video playing" check were dead for X11 players,
	 * and check_fullscreen_video rescheduled itself forever waiting. */
	if (c->isfullscreen && client_is_x11(c))
		track_client_frame(c);
	/* X11 mirror of commitnotify bookkeeping. */
	if (c->mon && client_is_x11(c)) {
		struct wlr_surface *cs = client_surface(c);

		c->mon->diag_commits_in++;
		if (cs && (cs->current.committed & WLR_SURFACE_STATE_BUFFER)) {
			c->mon->unsampled_buffer = 1;
			c->last_buffer_commit_ms = monotonic_msec();
			dynrender_committed(c);
		}
	}
	if (!c->scene_surface || !c->mon)
		return;
#ifdef XWAYLAND
	int iw, ih, nw, nh;

	if (!client_is_x11(c))
		return;
	client_get_committed_size(c, &nw, &nh);
	if (c->column) {
		/* X11 tiles: re-clip, re-send lost configures. */
		iw = c->geom.width  - 2 * (int)c->bw;
		ih = c->geom.height - 2 * (int)c->bw;
		client_clip_to_usable(c);
		if (!c->in_txn && iw > 0 && ih > 0 && nw > 0 && nh > 0 &&
				(nw != iw || nh != ih))
			client_request_size(c, iw, ih);
		return;
	}
	if (c->isfullscreen || c->is_notif || c->is_instrument ||
			client_is_unmanaged(c))
		return;
	/* Caught up: send the newest paced size. */
	if (nw == c->last_configured_w && nh == c->last_configured_h)
		client_flush_pending_size(c);
	float_fit(c);
#endif
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

		/* Static target: per-axis springs. */
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

		if (moved) {
			/* Round the far edge, not the size: a locked
			 * bottom/right edge (x and width springs cancelling)
			 * stays put instead of jittering ±1px from two
			 * independent truncations. */
			g.x = (int)c->geom_fx;
			g.y = (int)c->geom_fy;
			g.width = (int)(c->geom_fx + c->geom_fw) - g.x;
			g.height = (int)(c->geom_fy + c->geom_fh) - g.y;
			client_unfreeze(c);
			float_set(c, g, ANCHOR_MID, ANCHOR_MID);
			active = 1;
		} else {
			c->anim_active = 0;
			float_set(c, c->target_geom, ANCHOR_MID, ANCHOR_MID);
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
void
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
	c->frozen_buf_w = surface->buffer->base.width;
	c->frozen_buf_h = surface->buffer->base.height;

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
		if (c->mon != m || !client_surface(c) || c->frozen_buffer)
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
		if (client_surface(c)->mapped)
			client_freeze(c);
		/* No snapshot (unmapped, or mapped with no buffer — the
		 * FREEZE-SKIP case: X11 client that sat hidden on an inactive
		 * workspace, typically Steam): its tile slides in empty.  Flag
		 * it so rendermon drips frame_done every vblank through the
		 * camera-anim withhold — the client can then paint in step
		 * with the slide instead of popping in after settle. */
		if (!c->frozen_buffer)
			c->anim_drip = 1;
	}
}

static void
monitor_unfreeze_clients(Monitor *m, int include_x11, int include_wayland)
{
	Client *c;
	wl_list_for_each(c, &clients, link) {
		int is_x11;
		if (c->mon != m)
			continue;
		/* Anim over — the camera-anim withhold is gone, normal
		 * frame_done flow resumes. */
		c->anim_drip = 0;
		if (!c->frozen_buffer)
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
	m->tile_spring = 0;
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
					m->tile_spring = 1;
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
	{
		int wobble_still = 0;
		wobble_tick(m, dt, &wobble_still);
		if (wobble_still)
			active = 1;
	}

	/* Varsel-slide i høyre marg. Egen liste: override-redirect-klienter
	 * står ikke i `clients`, så clients_anim_tick ser dem aldri. */
	{
		int notif_still = 0;
		notify_tick(m, dt, &notif_still);
		if (notif_still)
			active = 1;
		notify_ghost_tick(m, dt, &notif_still);
		if (notif_still)
			active = 1;
		osd_tick(m, dt, &notif_still);
		if (notif_still)
			active = 1;
		notifyd_tick(m, dt, &notif_still);
		if (notif_still)
			active = 1;
		overview_tick(m, dt, &notif_still);
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

	/* Snapshot X11 on pure slides only. */
	{
		int pos_only = active && !size_anim;
		if (pos_only && !m->pos_anim_was_active)
			monitor_freeze_clients(m, /*x11=*/1, /*wl=*/0);
		if (!pos_only && m->pos_anim_was_active)
			monitor_unfreeze_clients(m, /*x11=*/1, /*wl=*/0);
		m->pos_anim_was_active = pos_only;
	}
	/* Pure slides pause client rendering. */
	m->camera_anim_active = camera_anim && !size_anim &&
			!live_resize_active() && !m->txn_active;

	/* Anim over: flush the position to every X11 client this monitor
	 * moved.  A camera slide / column reflow with no size change never
	 * reconfigures them (see client_flush_x11_pos), and the per-client
	 * settle above only covers clients that had anim_active — column
	 * clients moving at unchanged size take client_set_target_geom's
	 * anim_active = 0 branch and never get there. */
	if (m->anim_was_active && !active) {
		Client *c;
		wl_list_for_each(c, &clients, link)
			if (c->mon == m)
				client_flush_x11_pos(c);
	}

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
