#include "nixlytile.h"
#include "client.h"
#include "diag.h"

/* Max wait for slow clients. */
#define TXN_TIMEOUT_NS 200000000ULL

/* Render owed, not yet presumed lost. */
int
txn_owes(Client *c)
{
	return c->txn_owed && get_time_ns() - c->txn_sent_ns < TXN_TIMEOUT_NS;
}

static int
same_size(struct wlr_box a, struct wlr_box b)
{
	return a.width == b.width && a.height == b.height;
}

static int
in_pass(Client *c, Monitor *m)
{
	return c->mon == m && c->column && c->tile_ws_id &&
			c->tile_pass == m->tile_pass;
}

static int
on_screen(Client *c, struct wlr_box b)
{
	struct wlr_box out;

	b.x += c->tile_ox;
	b.y += c->tile_oy;
	return wlr_box_intersection(&out, &b, &c->mon->m);
}

static void
show(Client *c)
{
	struct wlr_box g = c->tile_shown;

	g.x += c->tile_ox;
	g.y += c->tile_oy;
	client_place(c, g);
	if (!c->in_txn && !txn_owes(c) && !c->mon->anim_was_active)
		client_flush_x11_pos(c);
}

static void
send_size(Client *c, int w, int h)
{
#ifdef XWAYLAND
	if (client_is_x11(c)) {
		client_set_size(c, (uint32_t)w, (uint32_t)h);
		return;
	}
#endif
	c->txn_serial = wlr_xdg_toplevel_set_size(c->surface.xdg->toplevel, w, h);
	c->resize = c->txn_serial;
}

/* Size of the commit being made. */
static void
pending_size(Client *c, int *w, int *h)
{
	struct wlr_surface *s = client_surface(c);

	*w = s->pending.width;
	*h = s->pending.height;
#ifdef XWAYLAND
	if (client_is_x11(c))
		return;
#endif
	if (c->surface.xdg->pending.geometry.width > 0) {
		*w = c->surface.xdg->pending.geometry.width;
		*h = c->surface.xdg->pending.geometry.height;
	}
}

/* Newest content: held or committed. */
static void
content_size(Client *c, int *w, int *h)
{
	*w = c->content_w;
	*h = c->content_h;
	if (!*w)
		client_get_committed_size(c, w, h);
}

/* Returns 1 if a render is owed. */
static int
configure(Client *c, struct wlr_box box)
{
	int w = MAX(box.width - 2 * (int)c->bw, 1);
	int h = MAX(box.height - 2 * (int)c->bw, 1);
	int cw, ch;

	content_size(c, &cw, &ch);
	client_set_bounds(c, box.width, box.height);
	c->txn_w = c->pending_resize_w = c->last_configured_w = w;
	c->txn_h = c->pending_resize_h = c->last_configured_h = h;
	send_size(c, w, h);
	converge_kick(c);
	c->txn_sent_ns = get_time_ns();
	c->txn_owed = cw != w || ch != h;
	return c->txn_owed;
}

/* Drag or spring: no hold. */
static int
loose(Monitor *m)
{
	return !m->txn_active && (m->tile_spring ||
			cursor_mode == CurResize || cursor_mode == CurColResize);
}

static int
fits(Client *c, struct wlr_box box)
{
	int cw, ch;

	content_size(c, &cw, &ch);
	return cw == box.width - 2 * (int)c->bw &&
			ch == box.height - 2 * (int)c->bw;
}

/* Box tracks target; content follows. */
static void
chase(Client *c)
{
	c->tile_shown = c->tile_target;
	if (!txn_owes(c) && !c->fs_hold && !fits(c, c->tile_target))
		configure(c, c->tile_target);
}

/* Late clients: one configure in flight. */
static void
catch_up(Client *c, struct wlr_box box)
{
	if (!txn_owes(c))
		configure(c, box);
}

static int
waiting(Monitor *m)
{
	Client *c;

	wl_list_for_each(c, &clients, link)
		if (c->mon == m && c->txn_wait)
			return 1;
	return 0;
}

static int
needed(Monitor *m)
{
	Client *c;

	wl_list_for_each(c, &clients, link)
		if (in_pass(c, m) && !c->tile_fresh &&
				!same_size(c->tile_target, c->tile_shown))
			return 1;
	return 0;
}

static void
apply(Monitor *m)
{
	Client *c;

	m->txn_active = 0;
	wl_list_for_each(c, &clients, link) {
		if (c->mon != m || !c->in_txn)
			continue;
		c->in_txn = 0;
		c->tile_shown = c->tile_txn;
		if (c->txn_wait)
			diag_logf("TILE", "TXN-TIMEOUT appid='%s' owed=%dx%d",
				client_get_appid(c) ? client_get_appid(c) : "(null)",
				c->txn_w, c->txn_h);
		c->txn_late |= c->txn_wait;
		c->txn_wait = 0;
		show(c);
	}
}

static void
begin(Monitor *m)
{
	Client *c;
	int wait = 0;

	m->txn_active = 1;
	m->txn_start_ns = get_time_ns();
	wl_list_for_each(c, &clients, link) {
		if (!in_pass(c, m) || c->tile_fresh)
			continue;
		c->in_txn = 1;
		c->tile_txn = c->tile_target;
		if (same_size(c->tile_txn, c->tile_shown) || c->fs_hold)
			continue;
		if (c->txn_late || (!on_screen(c, c->tile_shown) &&
				!on_screen(c, c->tile_txn))) {
			catch_up(c, c->tile_txn);
			continue;
		}
		c->txn_wait = configure(c, c->tile_txn);
		wait |= c->txn_wait;
	}
	if (!wait)
		apply(m);
}

/* Show finished boxes; next starts on vblank. */
static void
settle(Monitor *m)
{
	Client *c, *tmp;

	apply(m);
	wl_list_for_each_safe(c, tmp, &clients, link) {
		if (c->mon != m || !c->txn_locked)
			continue;
		c->txn_locked = 0;
		wlr_surface_unlock_cached(client_surface(c), c->txn_lock_seq);
	}
	wlr_output_schedule_frame(m->wlr_output);
}

void
txn_set_target(Client *c, struct wlr_box box, int ox, int oy)
{
	uint64_t ws_id = c->column->ws->window_id;
	int bw = (int)c->bw;
	int cw, ch;

	c->tile_target = box;
	c->tile_ox = ox;
	c->tile_oy = oy;
	c->tile_pass = c->mon->tile_pass;
	if (c->tile_ws_id == ws_id)
		return;
	c->tile_ws_id = ws_id;
	content_size(c, &cw, &ch);
	/* Already on screen: resize like any tile. */
	if (c->last_size_w && cw > 0 && ch > 0 &&
			(cw != box.width - 2 * bw || ch != box.height - 2 * bw)) {
		c->tile_shown = (struct wlr_box){ c->geom.x - ox, c->geom.y - oy,
				cw + 2 * bw, ch + 2 * bw };
		return;
	}
	c->tile_shown = box;
	c->tile_fresh = 1;
}

void
txn_place(Monitor *m)
{
	Client *c;
	int cw, ch;
	int drag = loose(m);
	int hold = !drag && (m->txn_active || needed(m));

	wl_list_for_each(c, &clients, link) {
		if (!in_pass(c, m))
			continue;
		if (c->tile_fresh) {
			c->tile_fresh = 0;
			content_size(c, &cw, &ch);
			c->txn_late = cw != c->tile_shown.width - 2 * (int)c->bw ||
					ch != c->tile_shown.height - 2 * (int)c->bw;
			if (c->txn_late)
				configure(c, c->tile_shown);
		} else if (drag) {
			chase(c);
		} else if (!hold) {
			c->tile_shown = c->tile_target;
		}
		show(c);
	}
}

int
txn_tick(Monitor *m)
{
	if (m->txn_active && (!waiting(m) ||
			get_time_ns() - m->txn_start_ns >= TXN_TIMEOUT_NS))
		settle(m);
	/* Begin on vblank: freshest target. */
	if (!m->txn_active && !loose(m) && needed(m))
		begin(m);
	return m->txn_active;
}

void
txn_forget(Client *c)
{
	c->tile_ws_id = 0;
	c->in_txn = c->txn_wait = c->txn_late = c->txn_owed = 0;
	c->content_w = c->content_h = 0;
	if (!c->txn_locked)
		return;
	c->txn_locked = 0;
	wlr_surface_unlock_cached(client_surface(c), c->txn_lock_seq);
}

void
txncommitnotify(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, txn_commit);
	struct wlr_box box;

	/* Held or about to apply: next content. */
	if (client_surface(c)->pending.committed & WLR_SURFACE_STATE_BUFFER)
		pending_size(c, &c->content_w, &c->content_h);
	if (fshold_commit(c))
		return;
	if (!c->txn_owed ||
			!client_commit_answers(c, c->txn_serial, c->txn_w, c->txn_h))
		return;
	c->txn_owed = 0;
	if (c->txn_wait) {
		c->txn_wait = 0;
		if (!waiting(c->mon)) {
			settle(c->mon);
			return;
		}
		c->txn_lock_seq = wlr_surface_lock_pending(client_surface(c));
		c->txn_locked = 1;
		return;
	}
	/* Late or loose: resync to the box. */
	box = c->in_txn ? c->tile_txn : c->tile_shown;
	c->txn_late = c->content_w != box.width - 2 * (int)c->bw ||
			c->content_h != box.height - 2 * (int)c->bw;
	if (c->txn_late)
		configure(c, box);
}
