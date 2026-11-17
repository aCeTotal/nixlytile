#include "nixlytile.h"
#include "client.h"
#include "diag.h"

static int
same_size(struct nx_box a, struct nx_box b)
{
	return a.width == b.width && a.height == b.height;
}

static int
in_pass(Client *c, Monitor *m)
{
	return c->mon == m && c->column && c->tile_ws_id &&
			c->tile_pass == m->tile_pass;
}

/* How a pass steers tile sizes. */
enum steer { STEER_AIM, STEER_CHASE, STEER_HOLD };

static void
show(Client *c)
{
	struct nx_box g = c->tile_shown;

	g.x += c->tile_ox;
	g.y += c->tile_oy;
	if (c->fsx_active) {
		client_fs_transition_apply(c, g);
		return;
	}
	client_place(c, g);
	if (!c->in_txn && !tile_owes(c) && !c->mon->anim_was_active)
		client_flush_x11_pos(c);
}

/* Held tiles keep size, not position. */
static void
hold(Client *c)
{
	c->tile_shown.x = c->tile_target.x;
	c->tile_shown.y = c->tile_target.y;
}

/* First placement asks for its box. */
static void
place_fresh(Client *c)
{
	c->tile_fresh = 0;
	c->txn_late = !tile_meets(c, c->tile_shown);
	if (!c->txn_late)
		return;
	tile_configure(c, c->tile_shown);
}

/* Late clients: one configure in flight. */
static void
catch_up(Client *c, struct nx_box box)
{
	if (!tile_owes(c))
		tile_configure(c, box);
}

static int
waiting(Monitor *m)
{
	Client *c;

	wl_list_for_each(c, &clients, link)
		if (c->mon == m && (c->txn_wait || c->txn_settles))
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
		c->txn_settles = 0;
		c->tile_shown.width = c->tile_txn.width;
		c->tile_shown.height = c->tile_txn.height;
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
		if (c->txn_late || (!tile_on_screen(c, c->tile_shown) &&
				!tile_on_screen(c, c->tile_txn))) {
			catch_up(c, c->tile_txn);
			continue;
		}
		c->txn_wait = tile_configure(c, c->tile_txn);
		wait |= c->txn_wait;
	}
	if (!wait)
		apply(m);
}

/* Show boxes; next on vblank. */
static void
settle(Monitor *m)
{
	Client *c, *tmp;

	apply(m);
	wl_list_for_each_safe(c, tmp, &clients, link) {
		if (c->mon != m || !c->txn_locked)
			continue;
		c->txn_locked = 0;
		nx_surface_unlock_cached(client_surface(c), c->txn_lock_seq);
	}
	nx_output_schedule_frame(m->nx_output);
}

void
txn_set_target(Client *c, struct nx_box box, int ox, int oy)
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
	tile_content(c, &cw, &ch);
	/* On screen: resize normally. */
	if (c->last_size_w && cw > 0 && ch > 0 &&
			(cw != box.width - 2 * bw || ch != box.height - 2 * bw)) {
		c->tile_shown = (struct nx_box){ c->geom.x - ox, c->geom.y - oy,
				cw + 2 * bw, ch + 2 * bw };
		return;
	}
	c->tile_shown = box;
	c->tile_fresh = 1;
}

void
txn_place(Monitor *m)
{
	static void (*const steer[])(Client *) = {
		[STEER_AIM] = tile_aim,
		[STEER_CHASE] = tile_chase,
		[STEER_HOLD] = hold,
	};
	enum steer mode = STEER_AIM;
	Client *c;

	if (!m->txn_active && live_resize_active())
		mode = STEER_CHASE;
	else if (!m->tile_spring && (m->txn_active || needed(m)))
		mode = STEER_HOLD;
	wl_list_for_each(c, &clients, link) {
		if (!in_pass(c, m))
			continue;
		if (c->tile_fresh)
			place_fresh(c);
		else
			steer[mode](c);
		show(c);
	}
}

/* Paced sizes go out without motion. */
static int
drag_tick(Monitor *m)
{
	Client *c;
	int lag = 0;

	wl_list_for_each(c, &clients, link) {
		if (!in_pass(c, m) || c->tile_fresh)
			continue;
		tile_follow(c, c->tile_shown);
		lag |= x11_paced(c) && !tile_fits(c, c->tile_shown);
	}
	return lag;
}

int
txn_tick(Monitor *m)
{
	if (m->txn_active && (!waiting(m) || m->tile_spring ||
			live_resize_active() ||
			get_time_ns() - m->txn_start_ns >= TXN_TIMEOUT_NS))
		settle(m);
	if (live_resize_active())
		return drag_tick(m);
	/* Begin on vblank: freshest target. */
	if (!m->txn_active && !m->tile_spring && needed(m))
		begin(m);
	return m->txn_active;
}

void
txn_forget(Client *c)
{
	c->tile_ws_id = 0;
	c->in_txn = c->txn_wait = c->txn_late = c->txn_owed = 0;
	c->txn_settles = 0;
	c->txn_w = c->txn_h = 0;
	c->content_w = c->content_h = 0;
	if (!c->txn_locked)
		return;
	c->txn_locked = 0;
	nx_surface_unlock_cached(client_surface(c), c->txn_lock_seq);
}

/* Last answer settles once applied. */
void
txn_applied(Client *c)
{
	if (!c->txn_settles)
		return;
	c->txn_settles = 0;
	if (c->mon && c->mon->txn_active && !waiting(c->mon))
		settle(c->mon);
}

/* Last answer settles, others lock. */
static void
waiter_answered(Client *c)
{
	c->txn_wait = 0;
	if (!waiting(c->mon)) {
		c->txn_settles = 1;
		return;
	}
	c->txn_lock_seq = nx_surface_lock_pending(client_surface(c));
	c->txn_locked = 1;
}

/* Box a late client meets. */
static struct nx_box
resync_box(Client *c)
{
	if (c->in_txn)
		return c->tile_txn;
	if (live_resize_active())
		return c->tile_shown;
	return c->tile_final;
}

void
txncommitnotify(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, txn_commit);
	struct nx_box box;

	/* Next content, held or applying. */
	if (client_surface(c)->pending.committed & NX_SURFACE_STATE_BUFFER)
		tile_pending_size(c, &c->content_w, &c->content_h);
	if (fshold_commit(c))
		return;
	if (!c->txn_owed ||
			!client_commit_answers(c, c->txn_serial, c->txn_w, c->txn_h))
		return;
	c->txn_owed = 0;
	if (c->txn_wait) {
		waiter_answered(c);
		return;
	}
	box = resync_box(c);
	c->txn_late = !tile_meets(c, box);
	tile_follow(c, box);
}

/* Tile size belongs to txn now. */
int
txn_owns(Client *c)
{
	return c->in_txn || c->morph_live || tile_owes(c) ||
			(c->mon && c->mon->tile_spring) || live_resize_active();
}
