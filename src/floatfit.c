#include "nixlytile.h"
#include "client.h"

static int
anchor(int pos, int span, int size, int how)
{
	if (how == ANCHOR_END)
		return pos + span - size;
	if (how == ANCHOR_MID)
		return pos + (span - size) / 2;
	return pos;
}

/* Box follows content, never stretched. */
void
float_fit(Client *c)
{
	struct wlr_box g;
	int cw, ch;

	if (!c->scene || !client_surface(c)->mapped)
		return;
	if (!c->float_want_set) {
		c->float_want = c->geom;
		c->float_ax = c->float_ay = ANCHOR_START;
		c->float_want_set = 1;
	}
	client_get_committed_size(c, &cw, &ch);
	if (cw <= 0 || ch <= 0) {
		cw = c->float_want.width - 2 * (int)c->bw;
		ch = c->float_want.height - 2 * (int)c->bw;
	}
	g.width = cw + 2 * (int)c->bw;
	g.height = ch + 2 * (int)c->bw;
	g.x = anchor(c->float_want.x, c->float_want.width, g.width, c->float_ax);
	g.y = anchor(c->float_want.y, c->float_want.height, g.height, c->float_ay);
	client_place(c, g);
	/* Position-only sync; sizes ride configures. */
	if (g.width == c->float_want.width && g.height == c->float_want.height &&
			!c->anim_active && !(c->mon && c->mon->anim_was_active))
		client_flush_x11_pos(c);
}

void
float_set(Client *c, struct wlr_box want, int ax, int ay)
{
	c->float_want = want;
	c->float_ax = ax;
	c->float_ay = ay;
	c->float_want_set = 1;
	client_set_bounds(c, want.width, want.height);
	/* X11 configures carry the wanted position. */
	c->geom = want;
	client_request_size(c, want.width - 2 * (int)c->bw,
			want.height - 2 * (int)c->bw);
	float_fit(c);
}
