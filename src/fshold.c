#include "nixlytile.h"
#include "client.h"

/* Grace for apps slow to resize. */
#define FSHOLD_TIMEOUT_NS 500000000ULL

/* Box the client gets after switching; 0 if unknown. */
static int
switch_box(Client *c, int on, struct wlr_box *box)
{
	int w, h;

	if (on) {
		*box = client_fullscreen_geom(c);
		return 1;
	}
	/* Floating exits animate in step with content. */
	if (c->isfloating)
		return 0;
	workspace_new_column_inner_size(c->mon, (int)borderpx, &w, &h);
	*box = (struct wlr_box){ 0, 0, w + 2 * (int)borderpx, h + 2 * (int)borderpx };
	return w > 0 && h > 0;
}

/* Games keep their launch cover and rules. */
static int
holdable(Client *c, int on)
{
	Workspace *ws;

	if (!c->mon || !client_surface(c)->mapped || client_is_unmanaged(c))
		return 0;
	if (looks_like_game(c) || is_game_content(c))
		return 0;
	ws = on ? client_target_ws(c) : NULL;
	return !ws || ws == c->mon->active_ws;
}

/* Send state and size past every dedup. */
static void
tell(Client *c, int on, int w, int h)
{
	client_set_fullscreen(c, on);
	c->fs_w = c->pending_resize_w = c->last_configured_w = w;
	c->fs_h = c->pending_resize_h = c->last_configured_h = h;
	c->fs_hold_ns = get_time_ns();
#ifdef XWAYLAND
	if (client_is_x11(c)) {
		client_set_size(c, (uint32_t)w, (uint32_t)h);
		return;
	}
#endif
	c->fs_serial = c->resize = wlr_xdg_toplevel_set_size(
			c->surface.xdg->toplevel, w, h);
}

static void
apply(Client *c)
{
	int on = c->fs_hold == FS_ENTERING;

	c->fs_hold = FS_SETTLED;
	setfullscreen(c, on);
}

/* Switch once content of the new size exists. */
void
fullscreen_request(Client *c, int on)
{
	struct wlr_box box;
	int bw = on ? 0 : (int)borderpx;
	int cw, ch;

	if (c->fs_hold == FS_ENTERING || c->fs_hold == FS_LEAVING) {
		if ((c->fs_hold == FS_ENTERING) == on)
			return;
		/* Reversed before the switch: restore the client. */
		c->fs_hold = FS_REVERTING;
		c->fs_told = FS_SETTLED;
		tell(c, c->isfullscreen, c->geom.width - 2 * (int)c->bw,
				c->geom.height - 2 * (int)c->bw);
		return;
	}
	if (c->fs_hold == FS_REVERTING) {
		if (on == c->isfullscreen)
			return;
		fshold_cancel(c);
	}
	if (on == c->isfullscreen || !holdable(c, on) || !switch_box(c, on, &box)) {
		setfullscreen(c, on);
		return;
	}
	client_get_committed_size(c, &cw, &ch);
	if (cw == box.width - 2 * bw && ch == box.height - 2 * bw) {
		setfullscreen(c, on);
		return;
	}
	c->fs_hold = c->fs_told = on ? FS_ENTERING : FS_LEAVING;
	client_set_bounds(c, box.width, box.height);
	tell(c, on, box.width - 2 * bw, box.height - 2 * bw);
	wlr_output_schedule_frame(c->mon->wlr_output);
}

/* Returns 1 when the commit switched fullscreen. */
int
fshold_commit(Client *c)
{
	int answered;

	if (c->fs_hold == FS_SETTLED)
		return 0;
	answered = client_commit_answers(c, c->fs_serial, c->fs_w, c->fs_h);
	if (c->fs_hold != FS_REVERTING) {
		if (!answered)
			return 0;
		apply(c);
		return 1;
	}
	if (answered) {
		fshold_cancel(c);
		return 0;
	}
	/* Hide renders of the abandoned size. */
	if (c->content_w != c->geom.width - 2 * (int)c->bw ||
			c->content_h != c->geom.height - 2 * (int)c->bw)
		client_freeze(c);
	return 0;
}

int
fshold_tick(Monitor *m)
{
	Client *c, *tmp;
	uint64_t now = get_time_ns();
	int held = 0;

	wl_list_for_each_safe(c, tmp, &clients, link) {
		if (c->mon != m || c->fs_hold == FS_SETTLED)
			continue;
		if (now - c->fs_hold_ns < FSHOLD_TIMEOUT_NS) {
			held = 1;
			continue;
		}
		if (c->fs_hold == FS_REVERTING)
			fshold_cancel(c);
		else
			apply(c);
	}
	return held;
}

/* Drop a pending switch, keep what was told. */
void
fshold_cancel(Client *c)
{
	if (c->fs_hold == FS_REVERTING)
		client_unfreeze(c);
	c->fs_hold = FS_SETTLED;
}

void
fshold_forget(Client *c)
{
	fshold_cancel(c);
	c->fs_told = FS_SETTLED;
}
