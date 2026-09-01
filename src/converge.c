/*
 * converge.c — size-convergence watchdog.
 *
 * The layout drives tile sizes through a chain of dedups: resize()'s
 * no-change fast-path, client_request_size's last_configured_* compare,
 * the xdg ack gate (one configure in flight), and X11 drag pacing.  Each
 * one assumes the previous configure landed.  When one doesn't — an anim
 * cancelled mid-flight, an ack that never came because the client was
 * starved of frame callbacks, a client that reasserted its own size —
 * every layer downstream treats the wrong size as current and nothing
 * re-drives it.  The tile then stays stretched/cropped until unrelated
 * layout numbers change (open a window, switch workspace), which is
 * exactly the "frozen tile that unfreezes when I create a new tile"
 * symptom.
 *
 * This pass runs once per rendered frame and closes that hole: any tile
 * whose COMMITTED size disagrees with its box for longer than the grace
 * window gets the configure re-sent with every dedup bypassed.  A client
 * that cannot comply (declared minimum larger than the tile) is detected
 * via its min size and counted as converged, so it is never re-driven in
 * a loop; anything else that refuses after CONVERGE_MAX_TRIES is left
 * scaled to its box, which is at least visually correct.
 */
#include "nixlytile.h"
#include "client.h"
#include "diag.h"

/* Time a client gets to ack + commit a configure before we assume the
 * configure was lost.  Comfortably above a slow toolkit's relayout, well
 * below "the user notices". */
#define CONVERGE_GRACE_MS  150
/* Re-sends before we stop fighting the client and just scale its buffer
 * into the box. */
#define CONVERGE_MAX_TRIES 4

/* One axis is settled when the client committed the box — or, for a box
 * smaller than the client's declared minimum, when it committed that
 * minimum instead.  A client that honours its own minimum can never
 * reach the box, so counting that as a mismatch would re-drive it on
 * every resize; a client that ignores its minimum (Qt does) commits the
 * box and settles on the first branch. */
static int
axis_settled(int committed, int box, int min)
{
	return committed == box || (min > 0 && box < min && committed == min);
}

/* A tile this pass owns.  Excludes everything whose size is legitimately
 * not the box right now: animating tiles (the anim settle sends the
 * authoritative resize), live drags (the pointer owns the size), and
 * floating/fullscreen clients (placed deliberately, not tiled). */
static int
converge_candidate(Client *c, Monitor *m)
{
	struct wlr_surface *s;

	if (c->mon != m || !c->column || c->anim_active)
		return 0;
	if (c->isfloating || c->isfullscreen)
		return 0;
	if (cursor_mode == CurResize || cursor_mode == CurColResize)
		return 0;
	s = client_surface(c);
	if (!s || !s->mapped || !c->scene_surface)
		return 0;
	return c->geom.width > 2 * (int)c->bw && c->geom.height > 2 * (int)c->bw;
}

/* True while a client still owes us a commit at its tile size.  Used by
 * the frame-done drip in rendermon: a client that can't repaint can't
 * converge, so it must keep getting frame callbacks even while it is
 * covered or off-viewport.  A client we already gave up on is excluded:
 * it will never commit that size, and dripping it every vblank forever
 * is pure overhead. */
int
client_size_pending(Client *c)
{
	return c && c->converge_since != 0 && !c->converge_gave_up;
}

/* Give-up is latched against the box it was reached at.  A different box
 * is a real layout change (tile resize, ws move, bar toggle) that the
 * client may well be able to satisfy — re-arm the watchdog. */
static void
converge_clear(Client *c)
{
	c->converge_since = 0;
	c->converge_tries = 0;
	c->converge_gave_up = 0;
	c->converge_gave_up_w = c->converge_gave_up_h = 0;
	c->converge_applied = 0;
}

int
clients_converge_tick(Monitor *m)
{
	Client *c;
	uint32_t now;
	int pending = 0;

	if (!m)
		return 0;

	now = (uint32_t)monotonic_msec();

	wl_list_for_each(c, &clients, link) {
		int nw, nh, iw, ih, min_w, min_h;

		if (!converge_candidate(c, m)) {
			/* Not ours right now (animating, dragging, floating).
			 * Keep a latched give-up: the anim that runs on every
			 * focus switch would otherwise clear it and re-run the
			 * whole 4-configure burst against a client that already
			 * proved it cannot comply. */
			if (!c->converge_gave_up)
				converge_clear(c);
			continue;
		}

		iw = c->geom.width  - 2 * (int)c->bw;
		ih = c->geom.height - 2 * (int)c->bw;

		/* Latched give-up at this same box: keep the buffer stretched
		 * into the box (the client still renders at its own size) but
		 * report nothing pending — no vblank chain, no frame_done drip.
		 * Without this a client that structurally can't reach its box
		 * pins the output at full refresh for as long as it is open. */
		if (c->converge_gave_up) {
			if (iw == c->converge_gave_up_w &&
					ih == c->converge_gave_up_h) {
				/* Steady state: the scale/clip is already in
				 * place.  Re-apply only when the tile moved or
				 * the client committed a new natural size —
				 * client_scale_to_box walks the whole scene
				 * subtree, pure no-op waste every other frame
				 * (wlroots dedups the setters anyway). */
				client_get_committed_size(c, &nw, &nh);
				if (!c->converge_applied ||
						c->converge_applied_x != c->geom.x ||
						c->converge_applied_y != c->geom.y ||
						c->converge_applied_nat_w != nw ||
						c->converge_applied_nat_h != nh) {
					client_scale_to_box(c, iw, ih);
					client_clip_to_usable(c);
					c->converge_applied = 1;
					c->converge_applied_x = c->geom.x;
					c->converge_applied_y = c->geom.y;
					c->converge_applied_nat_w = nw;
					c->converge_applied_nat_h = nh;
				}
				continue;
			}
			converge_clear(c);
		}

		client_get_committed_size(c, &nw, &nh);
		client_get_min_size(c, &min_w, &min_h);
		if (axis_settled(nw, iw, min_w) && axis_settled(nh, ih, min_h)) {
			converge_clear(c);
			continue;
		}

		if (!c->converge_since) {
			/* First frame of the mismatch — the configure is
			 * probably in flight.  Start the clock. */
			c->converge_since = now ? now : 1;
			pending = 1;
			continue;
		}

		/* Whatever the client is showing does not fit the box: stretch
		 * its current buffer into it and crop to the usable area, so
		 * the tile looks right for every frame the mismatch lasts. */
		client_scale_to_box(c, iw, ih);
		client_clip_to_usable(c);

		if (now - c->converge_since < CONVERGE_GRACE_MS) {
			pending = 1;
			continue;
		}

		if (c->converge_tries >= CONVERGE_MAX_TRIES) {
			c->converge_gave_up = 1;
			c->converge_gave_up_w = iw;
			c->converge_gave_up_h = ih;
			diag_logf("TILE",
				"CONVERGE-GIVEUP appid='%s' box=%dx%d committed=%dx%d (kept scaled to box)",
				client_get_appid(c) ? client_get_appid(c) : "(null)",
				iw, ih, nw, nh);
			continue;
		}

		/* Re-drive.  Clearing last_configured_* and the outstanding-ack
		 * serial is the point: those are the two dedups that swallow a
		 * repeat of a size the client never actually reached.  If
		 * wlroots still holds this size as the client's current one,
		 * client_set_size sends nothing and the try count runs out into
		 * the give-up branch above — no configure loop either way. */
		c->last_configured_w = c->last_configured_h = 0;
		c->resize = 0;
		client_request_size(c, iw, ih);
		c->converge_tries++;
		c->converge_since = now ? now : 1;
		pending = 1;
		diag_logf("TILE",
			"CONVERGE appid='%s' box=%dx%d committed=%dx%d try=%d",
			client_get_appid(c) ? client_get_appid(c) : "(null)",
			iw, ih, nw, nh, c->converge_tries);
	}

	return pending;
}
