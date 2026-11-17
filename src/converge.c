/* Re-drives tiles whose size drifted. */
#include "nixlytile.h"
#include "client.h"
#include "diag.h"

/* Time to answer a configure. */
#define CONVERGE_GRACE_MS  150
/* Re-sends before cropping for good. */
#define CONVERGE_MAX_TRIES 4

/* Box ±1 px, or minimum. */
static int
axis_settled(int committed, int box, int min)
{
	return abs(committed - box) <= SIZE_SLACK_PX ||
			(min > 0 && box < min && committed == min);
}

/* Tiles this pass may re-drive. */
static int
converge_candidate(Client *c, Monitor *m)
{
	struct nx_surface *s;

	if (c->mon != m || !c->column || c->anim_active || c->fsx_active || m->txn_active ||
			txn_owns(c))
		return 0;
	if (c->isfloating || c->isfullscreen)
		return 0;
	s = client_surface(c);
	if (!s || !s->mapped || !c->scene_surface)
		return 0;
	return c->geom.width > 2 * (int)c->bw && c->geom.height > 2 * (int)c->bw;
}

/* Owes a resized commit. */
int
client_size_pending(Client *c)
{
	return c && ((c->converge_since != 0 && !c->converge_gave_up) ||
			tile_owes(c) || c->fs_hold != FS_SETTLED || c->morph_live);
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

/* Event-driven gate for the per-frame walk below.  A size mismatch can
 * only ARISE from (a) an outgoing configure (client_request_size), (b) a
 * client commit (animcommitnotify), or (c) a geometry move re-anchoring
 * a gave-up client's crop (monitor_apply_positions).  All three
 * call this; the tick then re-walks until everything reports settled and
 * clears the flag — so a fully settled desktop skips the whole
 * clients walk (with two protocol-size reads per client) every frame. */
void
converge_kick(Client *c)
{
	Monitor *m;

	if (c && c->mon) {
		c->mon->converge_dirty = 1;
		return;
	}
	wl_list_for_each(m, &mons, link)
		m->converge_dirty = 1;
}

int
clients_converge_tick(Monitor *m)
{
	Client *c;
	uint32_t now;
	int pending = 0;

	if (!m)
		return 0;
	if (!m->converge_dirty)
		return 0;

	now = (uint32_t)monotonic_msec();

	wl_list_for_each(c, &clients, link) {
		int nw, nh, iw, ih, min_w, min_h;

		if (!converge_candidate(c, m)) {
			/* Keep latched give-ups. */
			if (!c->converge_gave_up)
				converge_clear(c);
			/* Re-check once the owed render lapses. */
			pending |= c->mon == m && tile_owes(c);
			continue;
		}

		iw = c->geom.width  - 2 * (int)c->bw;
		ih = c->geom.height - 2 * (int)c->bw;

		/* Latched give-up at this box: crop, report idle. */
		if (c->converge_gave_up) {
			if (iw == c->converge_gave_up_w &&
					ih == c->converge_gave_up_h) {
				/* Re-crop only after a move or commit. */
				client_get_committed_size(c, &nw, &nh);
				if (!c->converge_applied ||
						c->converge_applied_x != c->geom.x ||
						c->converge_applied_y != c->geom.y ||
						c->converge_applied_nat_w != nw ||
						c->converge_applied_nat_h != nh) {
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

		/* Mismatch is cropped, never stretched. */
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
				"CONVERGE-GIVEUP appid='%s' box=%dx%d committed=%dx%d (kept cropped to box)",
				client_get_appid(c) ? client_get_appid(c) : "(null)",
				iw, ih, nw, nh);
			continue;
		}

		/* Re-drive past both size dedups. */
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

	m->converge_dirty = pending;
	return pending;
}
