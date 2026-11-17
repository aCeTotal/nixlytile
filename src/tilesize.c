/* Decides and requests tile sizes. */
#include "nixlytile.h"
#include "client.h"

/* X11 has no ack: pace drags. */
#define X11_RESIZE_PACE_MS 40

/* Render owed, not yet presumed lost. */
int
tile_owes(Client *c)
{
	return c->txn_owed && get_time_ns() - c->txn_sent_ns < TXN_TIMEOUT_NS;
}

int
tile_on_screen(Client *c, struct nx_box b)
{
	struct nx_box out;

	b.x += c->tile_ox;
	b.y += c->tile_oy;
	return nx_box_intersection(&out, &b, &c->mon->m);
}

static void
send_size(Client *c, int w, int h)
{
#ifdef XWAYLAND
	if (client_is_x11(c)) {
		client_set_size(c, (uint32_t)w, (uint32_t)h);
		c->x11_cfg_ms = (uint32_t)monotonic_msec();
		return;
	}
#endif
	c->txn_serial = nx_xdg_toplevel_set_size(c->surface.xdg->toplevel, w, h);
	c->resize = c->txn_serial;
}

/* Size of the commit being made. */
void
tile_pending_size(Client *c, int *w, int *h)
{
	struct nx_surface *s = client_surface(c);

	*w = s->pending.width;
	*h = s->pending.height;
#ifdef XWAYLAND
	if (client_is_x11(c))
		return;
#endif
	if (c->surface.xdg->pending.geometry.width <= 0)
		return;
	*w = c->surface.xdg->pending.geometry.width;
	*h = c->surface.xdg->pending.geometry.height;
}

/* Content size a tile box holds. */
static struct nx_box
inner(Client *c, struct nx_box box)
{
	return (struct nx_box){ 0, 0, box.width - 2 * (int)c->bw,
			box.height - 2 * (int)c->bw };
}

/* Newest content: held or committed. */
void
tile_content(Client *c, int *w, int *h)
{
	*w = c->content_w;
	*h = c->content_h;
	if (!*w)
		client_get_committed_size(c, w, h);
}

/* Nonzero: render owed. */
int
tile_configure(Client *c, struct nx_box box)
{
	struct nx_box in = inner(c, box);
	int w = MAX(in.width, 1);
	int h = MAX(in.height, 1);
	int cw, ch;

	tile_content(c, &cw, &ch);
	client_set_bounds(c, box.width, box.height);
	c->txn_w = c->pending_resize_w = c->last_configured_w = w;
	c->txn_h = c->pending_resize_h = c->last_configured_h = h;
	send_size(c, w, h);
	converge_kick(c);
	c->txn_sent_ns = get_time_ns();
	c->txn_owed = cw != w || ch != h;
	return c->txn_owed;
}

int
tile_fits(Client *c, struct nx_box box)
{
	struct nx_box in = inner(c, box);
	int cw, ch;

	tile_content(c, &cw, &ch);
	return cw == in.width && ch == in.height;
}

/* Close enough to count as answered. */
int
tile_meets(Client *c, struct nx_box box)
{
	struct nx_box in = inner(c, box);
	int cw, ch;

	tile_content(c, &cw, &ch);
	return abs(cw - in.width) <= SIZE_SLACK_PX &&
			abs(ch - in.height) <= SIZE_SLACK_PX;
}

static int
asked(Client *c, struct nx_box box)
{
	struct nx_box in = inner(c, box);

	return c->txn_w == in.width && c->txn_h == in.height;
}

/* Xwayland answers before the app repaints. */
int
x11_paced(Client *c)
{
#ifdef XWAYLAND
	return client_is_x11(c) && c->x11_cfg_ms &&
			(uint32_t)monotonic_msec() - c->x11_cfg_ms < X11_RESIZE_PACE_MS;
#else
	return 0;
#endif
}

/* One configure in flight. */
void
tile_follow(Client *c, struct nx_box box)
{
	struct nx_box in = inner(c, box);

	if (in.width <= 0 || in.height <= 0 || tile_owes(c) || c->fs_hold)
		return;
	if (asked(c, box) || tile_fits(c, box)) {
		/* Drop a stale paced size. */
		c->pending_resize_w = c->last_configured_w;
		c->pending_resize_h = c->last_configured_h;
		return;
	}
	if (!live_resize_active() || !x11_paced(c)) {
		tile_configure(c, box);
		return;
	}
	/* Release flush sends it. */
	c->pending_resize_w = in.width;
	c->pending_resize_h = in.height;
}

/* Box tracks the pointer; content follows. */
void
tile_chase(Client *c)
{
	c->tile_shown = c->tile_target;
	if (!c->fsx_active && (c->morph_live || !tile_fits(c, c->tile_target)))
		morph_track(c);
	tile_follow(c, c->tile_target);
}

static int
satisfiable(Client *c, struct nx_box in)
{
	int min_w, min_h;

	client_get_min_size(c, &min_w, &min_h);
	return in.width >= min_w && in.height >= min_h;
}

/* One final-size configure. */
void
tile_aim(Client *c)
{
	struct nx_box in = inner(c, c->tile_final);

	c->tile_shown = c->tile_target;
	if (c->fs_hold || in.width <= 0 || in.height <= 0 ||
			asked(c, c->tile_final))
		return;
	if (!tile_owes(c) && tile_fits(c, c->tile_final)) {
		c->txn_w = in.width;
		c->txn_h = in.height;
		return;
	}
	tile_configure(c, c->tile_final);
	if (!c->fsx_active && tile_on_screen(c, c->tile_shown) &&
			satisfiable(c, in))
		morph_expect(c);
}
