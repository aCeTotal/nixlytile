#include "nixlytile.h"

/*
 * Multi-monitor game span: a fullscreen game covers the union of every
 * enabled output instead of a single monitor.  The client is configured
 * to the combined resolution (e.g. 3x 2560x1440 side-by-side →
 * 7680x1440), the scene graph renders the correct slice on each output,
 * and the cursor may travel across every spanned screen (the fullscreen
 * cursor confinement in input.c clamps to span_geom() instead of the
 * home monitor).
 *
 * THE RESOLUTION THE GAME PICKS DECIDES.  With `game-span "auto"`, a
 * game that requests a resolution its home monitor cannot hold (wider
 * or taller than the monitor, e.g. a surround res like 5760x1080)
 * spans automatically; picking a normal single-monitor resolution
 * unspans back to one screen.  See span_update_from_request(), driven
 * from configurex11's fullscreen size handling.  The `togglegamespan`
 * bind forces span on regardless (for games that cannot request a
 * larger-than-monitor size themselves); pressing it again returns to
 * resolution-driven behavior.
 *
 * The client keeps c->mon as its home monitor (workspace binding,
 * RandR primary, focus), only its fullscreen geometry changes.
 */

int game_span_auto = 0;

/* Union of all enabled, non-mirror outputs in layout coordinates. */
struct wlr_box
span_geom(void)
{
	Monitor *m;
	struct wlr_box box = {0};
	int have = 0;

	wl_list_for_each(m, &mons, link) {
		if (!m->wlr_output || !m->wlr_output->enabled || m->is_mirror)
			continue;
		if (m->m.width <= 0 || m->m.height <= 0)
			continue;
		if (!have) {
			box = m->m;
			have = 1;
			continue;
		}
		int x2 = box.x + box.width;
		int y2 = box.y + box.height;
		int mx2 = m->m.x + m->m.width;
		int my2 = m->m.y + m->m.height;
		if (m->m.x < box.x) box.x = m->m.x;
		if (m->m.y < box.y) box.y = m->m.y;
		if (mx2 > x2) x2 = mx2;
		if (my2 > y2) y2 = my2;
		box.width = x2 - box.x;
		box.height = y2 - box.y;
	}
	if (!have && selmon)
		return selmon->m;
	return box;
}

/* Span needs at least two enabled non-mirror outputs to mean anything. */
int
span_available(void)
{
	Monitor *m;
	int n = 0;

	wl_list_for_each(m, &mons, link)
		if (m->wlr_output && m->wlr_output->enabled && !m->is_mirror)
			n++;
	return n >= 2;
}

/* The fullscreen box for c: the whole layout when spanned, otherwise the
 * single-monitor (mirror-aware) geometry.  Every place that applies
 * fullscreen geometry goes through this so span survives monitor
 * hotplug (updatemons), X11 configure requests and setfullscreen. */
struct wlr_box
client_fullscreen_geom(Client *c)
{
	if (c && c->isspanned && span_available())
		return span_geom();
	return fullscreen_mirror_geom(c ? c->mon : selmon);
}

/* The mapped, visible fullscreen client that is currently spanning, if
 * any.  Used by the cursor-confinement path for monitors other than the
 * client's home monitor. */
Client *
spanned_fullscreen_client(void)
{
	Client *c;

	wl_list_for_each(c, &clients, link) {
		if (!c->isfullscreen || !c->isspanned)
			continue;
		if (!client_surface(c) || !client_surface(c)->mapped)
			continue;
		if (c->fs_ws && c->mon && c->fs_ws != c->mon->active_ws)
			continue;
		return c;
	}
	return NULL;
}

/* Turn off VRR on every output except the client's home monitor —
 * used when a span ends while the client stays fullscreen. */
static void
span_release_other_outputs(Client *c)
{
	Monitor *sm;

	wl_list_for_each(sm, &mons, link) {
		if (sm != c->mon && sm->wlr_output
				&& sm->wlr_output->enabled && !sm->is_mirror) {
			set_adaptive_sync(sm, 0);
			disable_game_vrr(sm);
		}
	}
}

/* Resolution-driven span (game-span "auto"): called from the X11
 * fullscreen configure path with the size the game just asked for.
 * Bigger than the home monitor (but within the span box) → span;
 * a normal single-monitor size → unspan.  Returns 1 if the span state
 * changed (caller must recompute fullscreen geometry). */
int
span_update_from_request(Client *c, int w, int h)
{
	struct wlr_box mon, sb;
	int wants;

	if (!game_span_auto || !c->isfullscreen || !c->mon || c->span_manual)
		return 0;
	if (!span_available()) {
		if (!c->isspanned)
			return 0;
		c->isspanned = 0;
		return 1;
	}
	mon = fullscreen_mirror_geom(c->mon);
	sb = span_geom();
	wants = (w > mon.width || h > mon.height)
		&& w <= sb.width && h <= sb.height;
	if (wants == c->isspanned)
		return 0;
	c->isspanned = wants;
	wlr_log(WLR_INFO, "span: %s appid='%s' requested=%dx%d mon=%dx%d span=%dx%d",
		wants ? "ENTER" : "EXIT",
		client_get_appid(c) ? client_get_appid(c) : "(null)",
		w, h, mon.width, mon.height, sb.width, sb.height);
	if (wants) {
		Monitor *sm;
		wl_list_for_each(sm, &mons, link)
			if (sm->wlr_output && sm->wlr_output->enabled && !sm->is_mirror) {
				set_adaptive_sync(sm, 1);
				enable_game_vrr(sm);
			}
	} else {
		span_release_other_outputs(c);
	}
	return 1;
}

void
togglegamespan(const Arg *arg)
{
	Client *sel = get_fullscreen_client();

	if (!sel)
		sel = focustop(selmon);
	if (!sel)
		return;
	if (!sel->span_manual && !span_available()) {
		osd_show(selmon, "Span: need 2+ monitors");
		return;
	}
	/* Manual override: force span on.  Second press releases the
	 * override — span then follows the game's requested resolution
	 * again (game-span "auto"), or turns off entirely. */
	sel->span_manual = !sel->span_manual;
	if (sel->span_manual) {
		sel->isspanned = 1;
	} else {
		sel->isspanned = 0;
		if (sel->isfullscreen)
			span_release_other_outputs(sel);
	}
	osd_show(sel->mon ? sel->mon : selmon,
		sel->span_manual ? "Game span: forced on"
		: (game_span_auto ? "Game span: auto (resolution-driven)"
		                  : "Game span: off"));
	/* Re-run setfullscreen with the current state to re-apply geometry
	 * (safe re-entry: detach/attach only fires on state transitions). */
	if (sel->isfullscreen)
		setfullscreen(sel, 1);
}
