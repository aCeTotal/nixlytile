#include "nixlytile.h"
#include "client.h"

/* ── Pending launch tracking ──────────────────────────────────────────
 * When the modal launcher or spawn() starts a program, we record the
 * child PID and the tags/monitor active at that moment.  When the new
 * window maps and applyrules() runs, we walk the client process's
 * parent chain to match it against a recorded launch PID — if found,
 * the window gets the launch-time tags instead of whatever tag the
 * user is currently viewing. */

typedef struct {
	pid_t pid;
	uint32_t tags;
	char output[32];
	uint64_t launch_ms;
} PendingLaunchEntry;

static PendingLaunchEntry pending_launches[MAX_PENDING_LAUNCHES];
static int pending_launch_count;

/* Walk /proc parent chain: return 1 if child is a descendant of ancestor */
static int
is_pid_descendant_of(pid_t child, pid_t ancestor)
{
	pid_t cur = child;
	char path[64], line[256];
	FILE *f;
	pid_t ppid;

	for (int depth = 0; depth < 32 && cur > 1; depth++) {
		if (cur == ancestor)
			return 1;
		snprintf(path, sizeof(path), "/proc/%d/status", (int)cur);
		f = fopen(path, "r");
		if (!f)
			return 0;
		ppid = 0;
		while (fgets(line, sizeof(line), f)) {
			if (strncmp(line, "PPid:\t", 6) == 0) {
				ppid = (pid_t)atoi(line + 6);
				break;
			}
		}
		fclose(f);
		if (ppid <= 1)
			return 0;
		cur = ppid;
	}
	return 0;
}

void
pending_launch_add(pid_t pid, uint32_t tags, const char *output_name)
{
	uint64_t now = monotonic_msec();
	int j = 0;

	/* Expire old entries */
	for (int i = 0; i < pending_launch_count; i++) {
		if (now - pending_launches[i].launch_ms < PENDING_LAUNCH_TIMEOUT_MS) {
			if (j != i)
				pending_launches[j] = pending_launches[i];
			j++;
		}
	}
	pending_launch_count = j;

	/* Drop oldest if full */
	if (pending_launch_count >= MAX_PENDING_LAUNCHES) {
		memmove(&pending_launches[0], &pending_launches[1],
			(MAX_PENDING_LAUNCHES - 1) * sizeof(PendingLaunchEntry));
		pending_launch_count--;
	}

	PendingLaunchEntry *pl = &pending_launches[pending_launch_count++];
	pl->pid = pid;
	pl->tags = tags;
	pl->launch_ms = now;
	snprintf(pl->output, sizeof(pl->output), "%s",
		output_name ? output_name : "");
}

int
pending_launch_find(pid_t client_pid, uint32_t *out_tags,
	char *out_output, size_t out_output_sz)
{
	uint64_t now = monotonic_msec();

	/* Search newest first — most likely match */
	for (int i = pending_launch_count - 1; i >= 0; i--) {
		PendingLaunchEntry *pl = &pending_launches[i];

		if (now - pl->launch_ms >= PENDING_LAUNCH_TIMEOUT_MS)
			continue;

		if (client_pid == pl->pid ||
		    is_pid_descendant_of(client_pid, pl->pid)) {
			*out_tags = pl->tags;
			if (out_output && out_output_sz > 0)
				snprintf(out_output, out_output_sz, "%s", pl->output);
			/* Do NOT remove the entry.  A single launch (e.g. Steam
			 * starting a game) can spawn several windows in quick
			 * succession — launcher overlay, the game itself,
			 * Proton/Wine dialogs — and all of them should land on
			 * the monitor that was active at launch time.  Entries
			 * expire naturally after PENDING_LAUNCH_TIMEOUT_MS. */
			return 1;
		}
	}
	return 0;
}

int
client_has_fullscreen_ancestor(Client *c)
{
	Client *p;
	int depth = 0;
	if (!c)
		return 0;
	p = client_get_parent(c);
	while (p && depth < 10) {
		if (p->isfullscreen)
			return 1;
		p = client_get_parent(p);
		depth++;
	}
	return 0;
}

void
applybounds(Client *c, struct wlr_box *bbox)
{
	/* set minimum possible */
	c->geom.width = MAX(1 + 2 * (int)c->bw, c->geom.width);
	c->geom.height = MAX(1 + 2 * (int)c->bw, c->geom.height);

	if (c->geom.x >= bbox->x + bbox->width)
		c->geom.x = bbox->x + bbox->width - c->geom.width;
	if (c->geom.y >= bbox->y + bbox->height)
		c->geom.y = bbox->y + bbox->height - c->geom.height;
	if (c->geom.x + c->geom.width <= bbox->x)
		c->geom.x = bbox->x;
	if (c->geom.y + c->geom.height <= bbox->y)
		c->geom.y = bbox->y;
}

void
applyrules(Client *c)
{
	/* rule matching */
	const char *appid, *title;
	uint32_t newtags = 0;
	int i;
	const Rule *r;
	Monitor *mon = selmon, *m;
	int mon_explicit = 0;

	appid = client_get_appid(c);
	title = client_get_title(c);

	wlr_log(WLR_INFO,
		"GAME_TRACE: applyrules enter appid='%s' title='%s' pid=%d "
		"geom=%dx%d selmon='%s'",
		appid ? appid : "(null)",
		title ? title : "(null)",
		(int)client_get_pid(c),
		c->geom.width, c->geom.height,
		selmon && selmon->wlr_output ? selmon->wlr_output->name : "(null)");

	for (r = rules; r < rules + nrules; r++) {
		if ((!r->title || strstr(title, r->title))
				&& (!r->id || strstr(appid, r->id))) {
			c->isfloating = r->isfloating;
			newtags |= r->tags;
			i = 0;
			wl_list_for_each(m, &mons, link) {
				if (r->monitor == i++) {
					mon = m;
					mon_explicit = 1;
				}
			}
		}
	}

	/* If no rule assigned specific tags, check if this window came
	 * from a launcher/spawn launch and use the tags that were active
	 * at launch time — so slow-starting apps land on the correct tag
	 * even if the user switched tags while waiting. */
	if (newtags == 0) {
		pid_t cpid = client_get_pid(c);
		if (cpid > 0) {
			uint32_t launch_tags;
			char launch_output[32] = {0};
			if (pending_launch_find(cpid, &launch_tags,
					launch_output, sizeof(launch_output))) {
				newtags = launch_tags;
				if (launch_output[0]) {
					wl_list_for_each(m, &mons, link) {
						if (strcmp(m->wlr_output->name, launch_output) == 0) {
							mon = m;
							mon_explicit = 1;
							break;
						}
					}
				}
				wlr_log(WLR_INFO,
					"applyrules: pending launch matched pid %d → "
					"tags=0x%x output=%s",
					(int)cpid, newtags, launch_output);
			}
		}
	}

	/* Steam-game fallback.  A Proton/Wine game launched from Steam
	 * typically maps with no useful rule match and no pending-launch
	 * entry (Steam launches the game process itself, not via our
	 * spawn() path).  If we leave it on selmon and selmon happens to
	 * be a different monitor than Steam is running on, the game
	 * initialises its swap chain at the wrong output's resolution —
	 * then updatemons() moves the window later, leaving the user with
	 * a mis-sized, mouse-clipped game.  Pin the game to Steam's
	 * monitor as a final fallback. */
	if (!mon_explicit && is_steam_game(c)) {
		Client *sc;
		wl_list_for_each(sc, &clients, link) {
			if (sc != c && sc->mon && is_steam_client(sc)) {
				mon = sc->mon;
				mon_explicit = 1;
				wlr_log(WLR_INFO,
					"applyrules: Steam-game fallback → "
					"placing pid=%d on Steam's monitor '%s'",
					(int)client_get_pid(c),
					mon->wlr_output ? mon->wlr_output->name : "(null)");
				break;
			}
		}
	}

	if (mon) {
		c->geom.x = (mon->w.width - c->geom.width) / 2 + mon->m.x;
		c->geom.y = (mon->w.height - c->geom.height) / 2 + mon->m.y;
	}

	c->isfloating |= client_is_float_type(c);
	wlr_log(WLR_INFO,
		"GAME_TRACE: applyrules exit → mon='%s' newtags=0x%x isfloating=%d",
		mon && mon->wlr_output ? mon->wlr_output->name : "(null)",
		newtags, c->isfloating);
	setmon(c, mon, newtags);
}

void
commitnotify(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, commit);

	if (c->surface.xdg->initial_commit) {
		/*
		 * Get the monitor this client will be rendered on
		 * Note that if the user set a rule in which the client is placed on
		 * a different monitor based on its title, this will likely select
		 * a wrong monitor.
		 */
		applyrules(c);
		if (c->mon) {
			client_set_scale(client_surface(c), c->mon->wlr_output->scale);
		}
		setmon(c, NULL, 0); /* Make sure to reapply rules in mapnotify() */

		wlr_xdg_toplevel_set_wm_capabilities(c->surface.xdg->toplevel,
				WLR_XDG_TOPLEVEL_WM_CAPABILITIES_FULLSCREEN);
		if (c->decoration)
			requestdecorationmode(&c->set_decoration_mode, c->decoration);
		wlr_xdg_toplevel_set_size(c->surface.xdg->toplevel, 0, 0);
		return;
	}

	/* mark a pending resize as completed */
	if (c->resize && c->resize <= c->surface.xdg->current.configure_serial) {
		c->resize = 0;
		/*
		 * If a newer size arrived while we waited for the ack, send it
		 * immediately instead of discarding it.  This prevents neighbor
		 * tiles from "lagging behind" during interactive resize — the
		 * client gets the latest requested geometry right away.
		 */
		if (c->pending_resize_w > 0 && c->pending_resize_h > 0 &&
		    (c->pending_resize_w != c->surface.xdg->toplevel->current.width ||
		     c->pending_resize_h != c->surface.xdg->toplevel->current.height)) {
			c->resize = client_set_size(c, c->pending_resize_w, c->pending_resize_h);
		}
		c->pending_resize_w = c->pending_resize_h = -1;
	}

	/* Track frame times for fullscreen clients (video detection) */
	if (c->isfullscreen)
		track_client_frame(c);

	/* For tiled clients in a tiling layout, use the geometry from btrtile
	 * (stored in old_geom) to ensure proper tiling. This prevents clients
	 * from appearing at their initial centered position before the layout
	 * has positioned them. */
	if (!c->isfloating && !c->isfullscreen && c->mon &&
	    c->mon->lt[c->mon->sellt]->arrange &&
	    c->old_geom.width > 0 && c->old_geom.height > 0) {
		resize(c, c->old_geom, 0);
	} else {
		resize(c, c->geom, (c->isfloating && !c->isfullscreen));
	}
}

void
createnotify(struct wl_listener *listener, void *data)
{
	/* This event is raised when a client creates a new toplevel (application window). */
	struct wlr_xdg_toplevel *toplevel = data;
	Client *c = NULL;

	/* Allocate a Client for this surface */
	c = toplevel->base->data = ecalloc(1, sizeof(*c));
	c->surface.xdg = toplevel->base;
	c->bw = borderpx;

	LISTEN(&toplevel->base->surface->events.commit, &c->commit, commitnotify);
	LISTEN(&toplevel->base->surface->events.map, &c->map, mapnotify);
	LISTEN(&toplevel->base->surface->events.unmap, &c->unmap, unmapnotify);
	LISTEN(&toplevel->events.destroy, &c->destroy, destroynotify);
	LISTEN(&toplevel->events.request_fullscreen, &c->fullscreen, fullscreennotify);
	LISTEN(&toplevel->events.request_maximize, &c->maximize, maximizenotify);
	LISTEN(&toplevel->events.set_title, &c->set_title, updatetitle);
}

void
destroydecoration(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, destroy_decoration);

	wl_list_remove(&c->destroy_decoration.link);
	wl_list_remove(&c->set_decoration_mode.link);
}

void
destroynotify(struct wl_listener *listener, void *data)
{
	/* Called when the xdg_toplevel is destroyed. */
	Client *c = wl_container_of(listener, c, destroy);
	Monitor *m;
	wl_list_remove(&c->destroy.link);
	wl_list_remove(&c->set_title.link);
	wl_list_remove(&c->fullscreen.link);
	/* Remove client from all monitors' layout trees and rebalance */
	wl_list_for_each(m, &mons, link)
		remove_client(m, c);
#ifdef XWAYLAND
	if (c->type != XDGShell) {
		wl_list_remove(&c->activate.link);
		wl_list_remove(&c->associate.link);
		wl_list_remove(&c->configure.link);
		wl_list_remove(&c->minimize.link);
		wl_list_remove(&c->dissociate.link);
		wl_list_remove(&c->set_hints.link);
		wl_list_remove(&c->set_override_redirect.link);
	} else
#endif
	{
		wl_list_remove(&c->commit.link);
		wl_list_remove(&c->map.link);
		wl_list_remove(&c->unmap.link);
		wl_list_remove(&c->maximize.link);
	}
	/* Safety net: if unmapnotify didn't clear game mode (e.g. rapid
	 * client destruction), catch it here before freeing */
	if (c == game_mode_client) {
		game_mode_pid = 0;
		game_mode_client = NULL;
	}
	free(c);
}

void
focusclient(Client *c, int lift)
{
	struct wlr_surface *old = seat->keyboard_state.focused_surface;
	int unused_lx, unused_ly, old_client_type;
	Client *old_c = NULL;
	LayerSurface *old_l = NULL;

	if (locked)
		return;

	/* Warp cursor to center of client if it is outside */
	if (lift)
		warpcursor(c);

	/* Raise client in stacking order if requested */
	if (c && lift)
		wlr_scene_node_raise_to_top(&c->scene->node);

	if (c && client_surface(c) == old)
		return;

	if ((old_client_type = toplevel_from_wlr_surface(old, &old_c, &old_l)) == XDGShell) {
		struct wlr_xdg_popup *popup, *tmp;
		wl_list_for_each_safe(popup, tmp, &old_c->surface.xdg->popups, link)
			wlr_xdg_popup_destroy(popup);
	}

	/* Put the new client atop the focus stack and select its monitor */
	if (c && !client_is_unmanaged(c)) {
		wl_list_remove(&c->flink);
		wl_list_insert(&fstack, &c->flink);
		selmon = c->mon;
		c->isurgent = 0;
		monitor_wake(c->mon);
		/* Invalidate fullscreen classification cache */
		c->mon->classify_cache_client = NULL;

		/* Don't change border color if there is an exclusive focus or we are
		 * handling a drag operation */
		if (!exclusive_focus && !seat->drag)
			client_set_border_color(c, focuscolor);
	}

	/* Deactivate old client if focus is changing */
	if (old && (!c || client_surface(c) != old)) {
		/* If an overlay is focused, don't focus or activate the client,
		 * but only update its position in fstack to render its border with focuscolor
		 * and focus it after the overlay is closed. */
		if (old_client_type == LayerShell && wlr_scene_node_coords(
					&old_l->scene->node, &unused_lx, &unused_ly)
				&& old_l->layer_surface->current.layer >= ZWLR_LAYER_SHELL_V1_LAYER_TOP) {
			return;
		} else if (old_c && old_c == exclusive_focus && client_wants_focus(old_c)) {
			return;
		/* Don't deactivate old client if the new one wants focus, as this causes issues with winecfg
		 * and probably other clients */
		} else if (old_c && !client_is_unmanaged(old_c) && (!c || !client_wants_focus(c))) {
			/* Don't deactivate a fullscreen client on another monitor —
			 * games may stop rendering when told they're inactive */
			if (!(old_c->isfullscreen && c && c->mon != old_c->mon)) {
				client_set_border_color(old_c, bordercolor);
				client_activate_surface(old, 0);
			}
		}
	}
	printstatus();

	if (!c) {
		/* With no client, all we have left is to clear focus */
		wlr_seat_keyboard_notify_clear_focus(seat);
		/* Notify text inputs about focus loss */
		text_input_focus_change(old, NULL);
		return;
	}

	/* Change cursor surface */
	motionnotify(0, NULL, 0, 0, 0, 0);

	/* Have a client, so focus its top-level wlr_surface */
	struct wlr_keyboard *kb = wlr_seat_get_keyboard(seat);
	wlr_log(WLR_INFO, "focusclient: giving keyboard focus to '%s', keyboard=%p",
		client_get_appid(c) ? client_get_appid(c) : "(null)", (void*)kb);
	client_notify_enter(client_surface(c), kb);

	/* Notify text inputs about focus change */
	text_input_focus_change(old, client_surface(c));

	/* Activate the new client */
	client_activate_surface(client_surface(c), 1);

	/* Update gamepad grab state - release for Steam/games, grab for HTPC views */
	gamepad_update_grab_state();
}

void
focusmon(const Arg *arg)
{
	int i = 0, nmons = wl_list_length(&mons);
	if (nmons) {
		do /* don't switch to disabled mons */
			selmon = dirtomon(arg->i);
		while (!selmon->wlr_output->enabled && i++ < nmons);
	}
	focusclient(focustop(selmon), 1);
}

void
warptomonitor(const Arg *arg)
{
	Monitor *m;
	int i = 0, nmons = wl_list_length(&mons);

	if (!nmons)
		return;

	m = selmon;
	do
		m = dirtomon(arg->i);
	while (!m->wlr_output->enabled && i++ < nmons);

	if (m && m->wlr_output->enabled) {
		/* Transfer any open status menus to new monitor */
		transfer_status_menus(selmon, m);
		selmon = m;
		/* Warp cursor to center of monitor */
		wlr_cursor_warp(cursor, NULL,
			m->m.x + m->m.width / 2,
			m->m.y + m->m.height / 2);
		focusclient(focustop(selmon), 1);
	}
}

Monitor *
monitorbyindex(unsigned int idx)
{
	Monitor *m;
	unsigned int i = 0;

	wl_list_for_each(m, &mons, link) {
		if (i == idx)
			return m;
		i++;
	}
	return NULL;
}

void
tagtomonitornum(const Arg *arg)
{
	Client *sel = focustop(selmon);
	Monitor *target;

	if (!sel)
		return;

	target = monitorbyindex(arg->ui);
	if (!target || target == sel->mon)
		return;

	setmon(sel, target, 0);
	free(sel->output);
	if (!(sel->output = strdup(sel->mon->wlr_output->name)))
		die("oom");
}

void
focusstack(const Arg *arg)
{
	/* Focus the next or previous client (in tiling order) on selmon */
	Client *c, *sel = focustop(selmon);
	if (!sel || (sel->isfullscreen && !client_has_children(sel)))
		return;
	if (arg->i > 0) {
		wl_list_for_each(c, &sel->link, link) {
			if (&c->link == &clients)
				continue; /* wrap past the sentinel node */
			if (VISIBLEON(c, selmon))
				break; /* found it */
		}
	} else {
		wl_list_for_each_reverse(c, &sel->link, link) {
			if (&c->link == &clients)
				continue; /* wrap past the sentinel node */
			if (VISIBLEON(c, selmon))
				break; /* found it */
		}
	}
	/* If only one client is visible on selmon, then c == sel */
	focusclient(c, 1);
}

Client *
focustop(Monitor *m)
{
	Client *c;
	wl_list_for_each(c, &fstack, flink) {
		if (VISIBLEON(c, m))
			return c;
	}
	return NULL;
}

void
fullscreennotify(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, fullscreen);
	int want = client_wants_fullscreen(c);
	wlr_log(WLR_INFO,
		"GAME_TRACE: fullscreennotify appid='%s' type=%s want=%d was=%d "
		"c->mon='%s'",
		client_get_appid(c) ? client_get_appid(c) : "(null)",
		c->type == X11 ? "X11" : "XDG",
		want, c->isfullscreen,
		c->mon && c->mon->wlr_output ? c->mon->wlr_output->name : "(null)");

	/*
	 * Games: handle client-initiated unfullscreen requests.
	 *
	 * compositor_fs == 1: Game was pre-fullscreened by the compositor at
	 *   map time.  Silently ignore the unfullscreen and clear compositor_fs
	 *   so subsequent requests are handled normally.  Telling Wine it's
	 *   unfullscreened (SOFT_UNFS) caused resolution cycling and broke
	 *   pointer grab setup.
	 *
	 * compositor_fs == 0: Game completed its own fullscreen init.  Block
	 *   further unfullscreen requests — Proton/Wine games frequently request
	 *   unfullscreen spuriously when losing focus or when the launcher spawns
	 *   helper windows.  The user can still toggle fullscreen via keybinding.
	 */
	if (!want && c->isfullscreen && (is_game_content(c) || looks_like_game(c))) {
		if (c->compositor_fs == 1) {
			wlr_log(WLR_INFO,
				"GAME_TRACE: fullscreennotify ignoring unfullscreen "
				"for pre-fullscreened game '%s' — clearing compositor_fs",
				client_get_appid(c) ? client_get_appid(c) : "(null)");
			game_log("GAME_FS: IGNORE_UNFS appid='%s' title='%s' "
				"reason=compositor_pre_fs_ignore",
				client_get_appid(c) ? client_get_appid(c) : "(null)",
				client_get_title(c) ? client_get_title(c) : "(null)");
			c->compositor_fs = 0;
			return;
		}
		wlr_log(WLR_INFO,
			"GAME_TRACE: fullscreennotify ignoring client unfullscreen "
			"for game '%s' — silently ignoring (no re-assert)",
			client_get_appid(c) ? client_get_appid(c) : "(null)");
		game_log("GAME_FS: IGNORE_UNFS appid='%s' title='%s' "
			"reason=game_unfullscreen_blocked",
			client_get_appid(c) ? client_get_appid(c) : "(null)",
			client_get_title(c) ? client_get_title(c) : "(null)");
		return;
	}

	/*
	 * Games already fullscreen requesting fullscreen again.
	 * Confirm without resize — the game is already centered at its
	 * chosen resolution via configurex11.
	 */
	if (want && c->isfullscreen && (is_game_content(c) || looks_like_game(c))) {
		wlr_log(WLR_INFO,
			"GAME_TRACE: fullscreennotify already fullscreen for "
			"game '%s' — confirming without resize",
			client_get_appid(c) ? client_get_appid(c) : "(null)");
		game_log("GAME_FS: CONFIRM appid='%s' title='%s' "
			"reason=already_fullscreen_reconfirm",
			client_get_appid(c) ? client_get_appid(c) : "(null)",
			client_get_title(c) ? client_get_title(c) : "(null)");
		client_set_fullscreen(c, 1);
		return;
	}

	/*
	 * Block fullscreen requests from splash/bootstrap game windows.
	 * Steam game launchers (EasyAntiCheat, Proton bootstrappers) set
	 * _NET_WM_STATE_FULLSCREEN on tiny windows.  Honouring the request
	 * would fullscreen a 600x200 surface inside a 3440x1440 frame —
	 * the content renders in a corner and the rest is white X11 bg.
	 * The game's real main window will request fullscreen later with a
	 * proper size.
	 */
	if (want && !c->isfullscreen && c->mon && looks_like_game(c)) {
		int cw = c->geom.width - 2 * c->bw;
		int ch = c->geom.height - 2 * c->bw;
		int mw = c->mon->m.width;
		int mh = c->mon->m.height;
		if (cw > 0 && ch > 0 && cw < mw / 3 && ch < mh / 3) {
			wlr_log(WLR_INFO,
				"GAME_TRACE: fullscreennotify blocking splash "
				"fullscreen for '%s' %dx%d (< %dx%d)",
				client_get_appid(c) ? client_get_appid(c) : "(null)",
				cw, ch, mw / 3, mh / 3);
			game_log("GAME_FS: BLOCK_SPLASH appid='%s' title='%s' "
				"size=%dx%d thresh=%dx%d reason=splash_too_small",
				client_get_appid(c) ? client_get_appid(c) : "(null)",
				client_get_title(c) ? client_get_title(c) : "(null)",
				cw, ch, mw / 3, mh / 3);
			client_set_fullscreen(c, 0);
			return;
		}
	}

	if (looks_like_game(c) || is_game_content(c))
		game_log("GAME_FS: %s appid='%s' title='%s' "
			"size=%dx%d mon='%s'",
			want ? "GRANT" : "REVOKE",
			client_get_appid(c) ? client_get_appid(c) : "(null)",
			client_get_title(c) ? client_get_title(c) : "(null)",
			c->geom.width, c->geom.height,
			c->mon && c->mon->wlr_output
				? c->mon->wlr_output->name : "(null)");
	setfullscreen(c, want);
}

void
killclient(const Arg *arg)
{
	Client *sel = focustop(selmon);
	if (sel)
		client_send_close(sel);
}

void
mapnotify(struct wl_listener *listener, void *data)
{
	/* Called when the surface is mapped, or ready to display on-screen. */
	Client *p = NULL;
	Client *w, *c = wl_container_of(listener, c, map);
	Monitor *m;
	int i;

	/* Create scene tree for this client and its border */
	{
		struct wlr_surface *surf = client_surface(c);
		if (!surf) {
			wlr_log(WLR_ERROR,
				"mapnotify: client_surface() returned NULL — "
				"surface may have been destroyed before mapping");
#ifdef XWAYLAND
			if (c->type == X11 && c->surface.xwayland)
				wlr_log(WLR_ERROR,
					"  X11 client class='%s' — check Xwayland log for errors",
					c->surface.xwayland->class ? c->surface.xwayland->class : "(null)");
#endif
			return;
		}

		c->scene = wlr_scene_tree_create(layers[LyrTile]);
		if (!c->scene) {
			wlr_log(WLR_ERROR,
				"mapnotify: wlr_scene_tree_create() failed — "
				"out of memory or scene graph error");
			return;
		}
		surf->data = c->scene;

		/* Enabled later by a call to arrange() */
		wlr_scene_node_set_enabled(&c->scene->node, client_is_unmanaged(c));

		c->scene_surface = c->type == XDGShell
				? wlr_scene_xdg_surface_create(c->scene, c->surface.xdg)
				: wlr_scene_subsurface_tree_create(c->scene, client_surface(c));
		if (!c->scene_surface) {
			wlr_log(WLR_ERROR,
				"mapnotify: scene surface creation failed — "
				"DMA-BUF or buffer import error. Check GPU driver and "
				"/dev/dri/ permissions");
			wlr_scene_node_destroy(&c->scene->node);
			c->scene = NULL;
			return;
		}
		c->scene->node.data = c->scene_surface->node.data = c;
	}

	/* Register with ext-foreign-toplevel-list for external tool visibility */
	if (foreign_toplevel_list) {
		struct wlr_ext_foreign_toplevel_handle_v1_state ftstate = {
			.title = client_get_title(c),
			.app_id = client_get_appid(c),
		};
		c->foreign_toplevel_handle = wlr_ext_foreign_toplevel_handle_v1_create(
			foreign_toplevel_list, &ftstate);
	}

	client_get_geometry(c, &c->geom);

	/* Read Steam X11 atoms (STEAM_GAME, STEAM_OVERLAY, STEAM_BIGPICTURE)
	 * before any game-detection logic runs.  These are the authoritative
	 * signals from Steam — see gamescope for the canonical consumer. */
	read_steam_properties(c);

	/* Save the game's preferred initial size BEFORE applyrules. The applyrules
	 * → setmon → arrange chain resizes c->geom to the tile geometry, losing
	 * the client's original preferred size. Used later for splash detection. */
	int initial_w = c->geom.width;
	int initial_h = c->geom.height;

	/* Handle unmanaged clients first so we can return prior create borders */
	if (client_is_unmanaged(c)) {
		/* Unmanaged clients always are floating */
		wlr_scene_node_reparent(&c->scene->node,
			layers[client_has_fullscreen_ancestor(c) ? LyrFS : LyrFloat]);
		wlr_scene_node_set_position(&c->scene->node, c->geom.x, c->geom.y);
		client_set_size(c, c->geom.width, c->geom.height);
		if (client_wants_focus(c)) {
			wlr_log(WLR_INFO, "mapnotify: unmanaged client wants focus, setting exclusive_focus");
			focusclient(c, 1);
			exclusive_focus = c;
		} else {
			wlr_log(WLR_INFO, "mapnotify: unmanaged client mapped but doesn't want focus");
		}
		goto unset_fullscreen;
	}

	for (i = 0; i < 4; i++) {
		c->border[i] = wlr_scene_rect_create(c->scene, 0, 0,
				c->isurgent ? urgentcolor : bordercolor);
		c->border[i]->node.data = c;
	}

	/* Initialize client geometry with room for border */
	client_set_tiled(c, WLR_EDGE_TOP | WLR_EDGE_BOTTOM | WLR_EDGE_LEFT | WLR_EDGE_RIGHT);
	c->geom.width += 2 * c->bw;
	c->geom.height += 2 * c->bw;

	/* Insert this client into client lists. */
	wl_list_insert(&clients, &c->link);
	wl_list_insert(&fstack, &c->flink);

	/* Debug: log client app_id */
	if (c->type == XDGShell && c->surface.xdg && c->surface.xdg->toplevel) {
		const char *aid = c->surface.xdg->toplevel->app_id;
		wlr_log(WLR_INFO, "mapnotify: XDG client mapped, app_id='%s'", aid ? aid : "(null)");
	}
#ifdef XWAYLAND
	if (c->type == X11 && c->surface.xwayland) {
		const char *cls = c->surface.xwayland->class;
		wlr_log(WLR_INFO, "mapnotify: X11 client mapped, class='%s'", cls ? cls : "(null)");
	}
#endif

	wlr_log(WLR_INFO,
		"GAME_TRACE: mapnotify enter appid='%s' pid=%d type=%s initial=%dx%d "
		"isfloating=%d isfullscreen=%d selmon='%s'",
		client_get_appid(c) ? client_get_appid(c) : "(null)",
		(int)client_get_pid(c),
		c->type == X11 ? "X11" : "XDG",
		initial_w, initial_h,
		c->isfloating, c->isfullscreen,
		selmon && selmon->wlr_output ? selmon->wlr_output->name : "(null)");

	/*
	 * Pre-detect main game windows BEFORE applyrules/setmon. This avoids
	 * the tile-flash race where applyrules→setmon→arrange() places the
	 * window at a tile position, followed by a later setfullscreen() that
	 * moves it to the fullscreen geometry. By pre-setting c->isfullscreen
	 * and bypassing applyrules for these windows, setmon's internal
	 * setfullscreen(c, 1) call applies the fullscreen geometry directly
	 * so only one frame is rendered — at the correct fullscreen size.
	 *
	 * We also force c->isfloating = 0 here, overriding applyrules and
	 * client_is_float_type() hints, because some games (Squad) expose
	 * float-type X11 hints on their main window.
	 */
	int pre_fullscreen_game = 0;
	int pre_game_splash = 0;    /* Game splash → float centered */
	Monitor *pre_target_mon = NULL;
	if (!c->isfloating && !c->isfullscreen && !client_is_unmanaged(c)
			&& client_get_parent(c) == NULL
			&& looks_like_game(c)) {
		pre_target_mon = selmon;
		/* Steam-game fallback: place on Steam's monitor */
		if (is_steam_game(c)) {
			Client *sc;
			wl_list_for_each(sc, &clients, link) {
				if (sc != c && sc->mon && is_steam_client(sc)) {
					pre_target_mon = sc->mon;
					break;
				}
			}
		}
		if (pre_target_mon) {
			const char *aid = client_get_appid(c);
			int mon_w = pre_target_mon->m.width;
			int mon_h = pre_target_mon->m.height;
			/*
			 * Detect confirmed Steam games via two signals:
			 *   1. STEAM_GAME X11 atom (set by Steam, read
			 *      into c->steam_game_id) — authoritative
			 *   2. steam_app_XXXXX WM_CLASS (set by Proton)
			 *      — fallback when atoms aren't set yet
			 */
			int is_confirmed_steam_game = 0;
#ifdef XWAYLAND
			if (c->steam_game_id > 0 && c->steam_game_id != 769
					&& !c->is_steam_overlay)
				is_confirmed_steam_game = 1;
#endif
			if (!is_confirmed_steam_game && aid
					&& strncasecmp(aid, "steam_app_", 10) == 0)
				is_confirmed_steam_game = 1;
			/* Native Wayland games won't have X11 atoms or
			 * steam_app_ class — check process ancestry */
			if (!is_confirmed_steam_game) {
				pid_t pid = client_get_pid(c);
				if (pid > 1 && is_game_launcher_child(pid))
					is_confirmed_steam_game = 1;
			}

			/*
			 * Splash threshold: for confirmed Steam games use
			 * 1/3 of monitor (conservative — only tiny splash
			 * logos).  For non-Steam, use 3/4 (original).
			 *
			 * On 3440x1440 ultrawide:
			 *   Steam: 1147x480 — 800x450 splash ✓, 1374x800 game ✗
			 *   Other: 2580x1080
			 */
			int thresh_w, thresh_h;
			if (is_confirmed_steam_game) {
				thresh_w = mon_w / 3;
				thresh_h = mon_h / 3;
			} else {
				thresh_w = (mon_w * 3) / 4;
				thresh_h = (mon_h * 3) / 4;
			}
			int small_w = initial_w > 0
				&& initial_w < thresh_w;
			int small_h = initial_h > 0
				&& initial_h < thresh_h;
			int is_small = small_w && small_h;

			if (is_small) {
				/*
				 * Game splash / bootstrap window.
				 * Show at its natural size, centered on the
				 * game's monitor.  No black fullscreen
				 * background; the desktop stays visible
				 * behind it.
				 *
				 * If the game later requests fullscreen via
				 * the X11 protocol, fullscreennotify() will
				 * honour that request normally.
				 */
				pre_game_splash = 1;
				c->isfloating = 1;
				c->geom.width = initial_w;
				c->geom.height = initial_h;
				c->geom.x = (mon_w - initial_w) / 2
					+ pre_target_mon->m.x;
				c->geom.y = (mon_h - initial_h) / 2
					+ pre_target_mon->m.y;
				wlr_log(WLR_INFO,
					"GAME_TRACE: game splash appid='%s' "
					"mon='%s' %dx%d centered@%d,%d",
					aid ? aid : "(null)",
					pre_target_mon->wlr_output
						? pre_target_mon->wlr_output->name
						: "(null)",
					initial_w, initial_h,
					c->geom.x, c->geom.y);
				game_log("GAME_SPLASH: appid='%s' title='%s' "
					"type=%s size=%dx%d centered@%d,%d "
					"mon='%s' steam_id=%d confirmed=%d "
					"thresh=%dx%d pid=%d",
					aid ? aid : "(null)",
					client_get_title(c) ? client_get_title(c) : "(null)",
					client_is_x11(c) ? "X11" : "XDG",
					initial_w, initial_h,
					c->geom.x, c->geom.y,
					pre_target_mon->wlr_output
						? pre_target_mon->wlr_output->name : "(null)",
					c->steam_game_id, is_confirmed_steam_game,
					thresh_w, thresh_h,
					(int)client_get_pid(c));
			} else {
				/*
				 * Game main window — large enough to be the
				 * actual game.  Immediate true fullscreen —
				 * the game will init its swap chain at the
				 * monitor's native resolution.
				 */
				pre_fullscreen_game = 1;
				c->isfullscreen = 1;
				c->isfloating = 0;
				c->compositor_fs = 1;
				wlr_log(WLR_INFO,
					"GAME_TRACE: pre-fullscreen "
					"appid='%s' mon='%s' initial=%dx%d",
					aid ? aid : "(null)",
					pre_target_mon->wlr_output
						? pre_target_mon->wlr_output->name
						: "(null)",
					initial_w, initial_h);
				game_log("GAME_MAP: appid='%s' title='%s' "
					"type=%s size=%dx%d fullscreen=1 "
					"mon='%s' steam_id=%d confirmed=%d "
					"thresh=%dx%d pid=%d",
					aid ? aid : "(null)",
					client_get_title(c) ? client_get_title(c) : "(null)",
					client_is_x11(c) ? "X11" : "XDG",
					initial_w, initial_h,
					pre_target_mon->wlr_output
						? pre_target_mon->wlr_output->name : "(null)",
					c->steam_game_id, is_confirmed_steam_game,
					thresh_w, thresh_h,
					(int)client_get_pid(c));
			}
		}
	}

	/* Set initial monitor, tags, floating status, and focus:
	 * we always consider floating, clients that have parent and thus
	 * we set the same tags and monitor as its parent.
	 * If there is no parent, apply rules */
	if ((pre_fullscreen_game || pre_game_splash) && pre_target_mon) {
		/* Skip applyrules so rule-based isfloating doesn't override our
		 * pre-set state, and so client_is_float_type() hints don't force
		 * the game into a floating state. */
		setmon(c, pre_target_mon, pre_target_mon->tagset[pre_target_mon->seltags]);
	} else if ((p = client_get_parent(c))) {
		wlr_log(WLR_INFO,
			"GAME_TRACE: mapnotify parent-path parent_appid='%s' parent_mon='%s'",
			client_get_appid(p) ? client_get_appid(p) : "(null)",
			p->mon && p->mon->wlr_output ? p->mon->wlr_output->name : "(null)");
		c->isfloating = 1;
		if (tray_anchor_time_ms && monotonic_msec() - tray_anchor_time_ms <= 1000) {
			c->geom.x = tray_anchor_x - c->geom.width / 2;
			c->geom.y = tray_anchor_y;
		} else if (p->mon) {
			c->geom.x = (p->mon->w.width - c->geom.width) / 2 + p->mon->m.x;
			c->geom.y = (p->mon->w.height - c->geom.height) / 2 + p->mon->m.y;
		}
		setmon(c, p->mon, p->tags);
	} else {
		wlr_log(WLR_INFO, "GAME_TRACE: mapnotify applyrules-path (no parent)");
		applyrules(c);
	}
	/* Ensure client has a valid monitor. If selmon is NULL (all outputs
	 * disconnected), the client can't be displayed - clean up and return. */
	if (!c->mon) {
		wlr_log(WLR_ERROR, "mapnotify: client '%s' has no monitor, assigning to selmon",
			client_get_appid(c) ? client_get_appid(c) : "(unknown)");
		if (selmon) {
			setmon(c, selmon, 0);
		} else {
			wlr_log(WLR_ERROR, "mapnotify: no monitor available, client cannot be displayed");
			return;
		}
	}
	free(c->output);
	c->output = strdup(c->mon->wlr_output->name);
	if (c->output == NULL)
		die("oom");

	/*
	 * HTPC mode Steam handling:
	 * - Force Steam main window to fullscreen on tag 4
	 * - Steam popups/dialogs stay floating but get focus and raised to top
	 */
	if (htpc_mode_active) {
		if (is_steam_client(c) && !c->isfloating) {
			/* Steam main window - place on tag 4 and fullscreen */
			wlr_log(WLR_INFO, "HTPC: Placing Steam on tag 4 and fullscreen");
			c->tags = 1 << 3; /* Tag 4 = bit 3 */
			setfullscreen(c, 1);
		} else if (is_steam_popup(c) || (p && is_steam_popup(p))) {
			/* Steam popup/dialog - ensure it's floating, centered, and focused */
			c->isfloating = 1;
			if (c->mon) {
				c->geom.x = (c->mon->w.width - c->geom.width) / 2 + c->mon->m.x;
				c->geom.y = (c->mon->w.height - c->geom.height) / 2 + c->mon->m.y;
			}
			/* Raise to top and focus */
			wlr_scene_node_raise_to_top(&c->scene->node);
			focusclient(c, 1);
			wlr_log(WLR_INFO, "HTPC: Steam popup raised and focused");
			/* Show cursor for popup interaction */
			nixly_cursor_set_xcursor("default");
		}
	}

	/*
	 * AUTO-FULLSCREEN FOR GAMES
	 *
	 * Decision is taken at map time based on the window's initial size:
	 *   - Small (< 3/4 of monitor in both dims) → centered-floating splash
	 *   - Otherwise → immediate true fullscreen
	 *
	 * setmon → setfullscreen already sent a configure at native monitor
	 * resolution.  Let it stand — DXVK/Wine handles internal resolution
	 * scaling via the Vulkan swapchain.  configurex11 will force native
	 * resolution for any subsequent configure requests from the game.
	 */
	if (pre_fullscreen_game) {
		wlr_log(WLR_INFO,
			"GAME_TRACE: pre-fullscreen game ready c->mon='%s' geom=%dx%d@%d,%d",
			c->mon && c->mon->wlr_output ? c->mon->wlr_output->name : "(null)",
			c->geom.width, c->geom.height, c->geom.x, c->geom.y);
		focusclient(c, 1);
		printstatus();
		goto unset_fullscreen;
	}

	if (pre_game_splash) {
		/* Game splash/bootstrap: show at natural size, centered
		 * on the game's monitor.  Use LyrOverlay so the splash is
		 * visible above any existing fullscreen game window from the
		 * same app — arrange() would otherwise disable it behind the
		 * fullscreen client.  The node is destroyed automatically
		 * when the splash unmaps.
		 *
		 * Return instead of goto unset_fullscreen: we don't want
		 * a splash to unfullscreen any running game. */
		wlr_scene_node_set_enabled(&c->scene->node, 1);
		wlr_scene_node_reparent(&c->scene->node, layers[LyrOverlay]);
		resize(c, c->geom, 1);
		focusclient(c, 1);
		printstatus();
		return;
	}

	if (!c->isfloating && !c->isfullscreen && looks_like_game(c)) {
		int mon_w = c->mon->m.width;
		int mon_h = c->mon->m.height;
		int small_w = initial_w > 0 && initial_w < (mon_w * 3) / 4;
		int small_h = initial_h > 0 && initial_h < (mon_h * 3) / 4;

		wlr_log(WLR_INFO,
			"GAME_TRACE: game detected appid='%s' c->mon='%s' mon=%dx%d@%d,%d "
			"initial=%dx%d small=%d,%d",
			client_get_appid(c) ? client_get_appid(c) : "(null)",
			c->mon && c->mon->wlr_output ? c->mon->wlr_output->name : "(null)",
			mon_w, mon_h, c->mon->m.x, c->mon->m.y,
			initial_w, initial_h, small_w, small_h);

		if (small_w && small_h) {
			wlr_log(WLR_INFO, "Game splash/logo detected: '%s' %dx%d, centering",
				client_get_appid(c) ? client_get_appid(c) : "(unknown)",
				initial_w, initial_h);
			c->isfloating = 1;
			c->geom.width = initial_w;
			c->geom.height = initial_h;
			c->geom.x = (mon_w - initial_w) / 2 + c->mon->m.x;
			c->geom.y = (mon_h - initial_h) / 2 + c->mon->m.y;
			wlr_scene_node_reparent(&c->scene->node, layers[LyrFloat]);
			resize(c, c->geom, 1);
			focusclient(c, 1);
			printstatus();
			return;
		}

		/* Main game window → direct true fullscreen, no delay. */
		wlr_log(WLR_INFO, "Game main window detected: '%s' %dx%d — true fullscreen",
			client_get_appid(c) ? client_get_appid(c) : "(unknown)",
			initial_w, initial_h);
		c->compositor_fs = 1;
		setfullscreen(c, 1);
		wlr_log(WLR_INFO,
			"GAME_TRACE: after setfullscreen c->mon='%s' geom=%dx%d@%d,%d",
			c->mon && c->mon->wlr_output ? c->mon->wlr_output->name : "(null)",
			c->geom.width, c->geom.height, c->geom.x, c->geom.y);
		focusclient(c, 1);
		printstatus();
		return;
	}

	printstatus();

unset_fullscreen:
	m = c->mon ? c->mon : xytomon(c->geom.x, c->geom.y);
	/* In HTPC mode, don't unset fullscreen for Steam popups appearing */
	if (htpc_mode_active && (is_steam_popup(c) || (p && is_steam_client(p))))
		return;
	{
		/* Don't unfullscreen another window of the SAME app — a game
		 * launcher spawning splashes/children would otherwise cause the
		 * main fullscreen window to flip in and out of fullscreen,
		 * destroying pointer constraints and breaking mouse input. */
		const char *new_appid = client_get_appid(c);
		wl_list_for_each(w, &clients, link) {
			if (w == c || w == p || !w->isfullscreen || m != w->mon
					|| !(w->tags & c->tags))
				continue;
			const char *old_appid = client_get_appid(w);
			if (new_appid && old_appid && strcmp(new_appid, old_appid) == 0) {
				wlr_log(WLR_INFO,
					"GAME_TRACE: unset_fullscreen skip same-app '%s'",
					new_appid);
				continue;
			}
			setfullscreen(w, 0);
		}
	}
}

void
maximizenotify(struct wl_listener *listener, void *data)
{
	/* This event is raised when a client would like to maximize itself,
	 * typically because the user clicked on the maximize button on
	 * client-side decorations. dwl doesn't support maximization, but
	 * to conform to xdg-shell protocol we still must send a configure.
	 * Since xdg-shell protocol v5 we should ignore request of unsupported
	 * capabilities, just schedule a empty configure when the client uses <5
	 * protocol version
	 * wlr_xdg_surface_schedule_configure() is used to send an empty reply. */
	Client *c = wl_container_of(listener, c, maximize);
	if (c->surface.xdg->initialized
			&& wl_resource_get_version(c->surface.xdg->toplevel->resource)
					< XDG_TOPLEVEL_WM_CAPABILITIES_SINCE_VERSION)
		wlr_xdg_surface_schedule_configure(c->surface.xdg);
}

void
setsticky(Client *c, int sticky)
{
	if (sticky && !c->issticky) {
		c->issticky = 1;
	} else if (!sticky && c->issticky) {
		c->issticky = 0;
		arrange(c->mon);
	}
}

void
resize(Client *c, struct wlr_box geo, int interact)
{
	struct wlr_box *bbox;
	struct wlr_box clip;
	int reqw, reqh;

	if (!c->mon || !client_surface(c)->mapped)
		return;

	bbox = interact ? &sgeom : &c->mon->w;

	client_set_bounds(c, geo.width, geo.height);
	c->geom = geo;
	applybounds(c, bbox);

	/* Update scene-graph, including borders */
	wlr_scene_node_set_position(&c->scene->node, c->geom.x, c->geom.y);
	wlr_scene_node_set_position(&c->scene_surface->node, c->bw, c->bw);
	wlr_scene_rect_set_size(c->border[0], c->geom.width, c->bw);
	wlr_scene_rect_set_size(c->border[1], c->geom.width, c->bw);
	wlr_scene_rect_set_size(c->border[2], c->bw, c->geom.height - 2 * c->bw);
	wlr_scene_rect_set_size(c->border[3], c->bw, c->geom.height - 2 * c->bw);
	wlr_scene_node_set_position(&c->border[1]->node, 0, c->geom.height - c->bw);
	wlr_scene_node_set_position(&c->border[2]->node, 0, c->bw);
	wlr_scene_node_set_position(&c->border[3]->node, c->geom.width - c->bw, c->bw);

	reqw = c->geom.width - 2 * c->bw;
	reqh = c->geom.height - 2 * c->bw;

	/*
	 * Avoid flooding heavy clients: only send a new configure when there isn't
	 * already one pending. While waiting for an ack we just remember the
	 * latest requested size.
	 */
	if (!c->resize)
		c->resize = client_set_size(c, reqw, reqh);
	c->pending_resize_w = reqw;
	c->pending_resize_h = reqh;

	client_get_clip(c, &clip);
	wlr_scene_subsurface_tree_set_clip(&c->scene_surface->node, &clip);
}

void
setfloating(Client *c, int floating)
{
	c->isfloating = floating;
	/* If in floating layout do not change the client's layer */
	if (!c->mon || !client_surface(c)->mapped || !c->mon->lt[c->mon->sellt]->arrange)
		return;
	wlr_scene_node_reparent(&c->scene->node, layers[c->isfullscreen ||
			client_has_fullscreen_ancestor(c) ? LyrFS
			: c->isfloating ? LyrFloat : LyrTile]);
	arrange(c->mon);
	printstatus();
}

/* Set by togglefullscreen() to distinguish user-initiated unfullscreen
 * from Wine/Proton-initiated unfullscreen that should be blocked. */
static int user_toggled_fullscreen;

void
setfullscreen(Client *c, int fullscreen)
{
	wlr_log(WLR_INFO,
		"GAME_TRACE: setfullscreen enter appid='%s' want=%d was=%d "
		"c->mon='%s' geom=%dx%d@%d,%d",
		client_get_appid(c) ? client_get_appid(c) : "(null)",
		fullscreen, c->isfullscreen,
		c->mon && c->mon->wlr_output ? c->mon->wlr_output->name : "(null)",
		c->geom.width, c->geom.height, c->geom.x, c->geom.y);

	/*
	 * In HTPC mode, games and Steam Big Picture are locked to fullscreen
	 * and cannot exit fullscreen. This prevents accidental unfullscreen
	 * from game menus or escape keys. The app must be closed to exit fullscreen.
	 */
	if (htpc_mode_active && !fullscreen && c->isfullscreen &&
	    (is_game_content(c) || is_steam_client(c))) {
		wlr_log(WLR_INFO, "HTPC: Blocking unfullscreen for '%s'",
			client_get_appid(c) ? client_get_appid(c) : "(unknown)");
		/* Re-assert fullscreen state to the client */
		client_set_fullscreen(c, 1);
		return;
	}

	/* Block Wine/Proton-initiated unfullscreen for game clients.
	 * Wine interprets the Super/Windows key as a toggle and sends
	 * unfullscreen requests that bypass fullscreennotify.  This causes
	 * fullscreen cycling, game mode toggling, and mouse input loss.
	 * User-initiated unfullscreen (Mod+f) sets user_toggled_fullscreen
	 * to bypass this guard. */
	if (!fullscreen && c->isfullscreen && !user_toggled_fullscreen &&
	    (is_game_content(c) || looks_like_game(c))) {
		wlr_log(WLR_INFO,
			"GAME_TRACE: setfullscreen blocking unfullscreen for game '%s'",
			client_get_appid(c) ? client_get_appid(c) : "(unknown)");
		client_set_fullscreen(c, 1);
		return;
	}

	c->isfullscreen = fullscreen;
	if (!c->mon || !client_surface(c)->mapped) {
		wlr_log(WLR_INFO,
			"GAME_TRACE: setfullscreen early-return mon=%p mapped=%d",
			(void*)c->mon,
			client_surface(c) ? client_surface(c)->mapped : -1);
		return;
	}

	c->bw = fullscreen ? 0 : borderpx;
	client_set_fullscreen(c, fullscreen);
	wlr_scene_node_reparent(&c->scene->node, layers[c->isfullscreen
			? LyrFS : c->isfloating ? LyrFloat : LyrTile]);

	/* Reparent floating children to match parent's layer */
	{
		Client *child;
		struct wlr_scene_tree *target = layers[fullscreen ? LyrFS : LyrFloat];
		wl_list_for_each(child, &clients, link) {
			if (child != c && child->isfloating && !child->isfullscreen
					&& client_get_parent(child) == c
					&& child->scene->node.parent != target) {
				wlr_scene_node_reparent(&child->scene->node, target);
			}
		}
	}

	if (fullscreen) {
		struct wlr_box fsgeom = fullscreen_mirror_geom(c->mon);
		wlr_log(WLR_INFO,
			"GAME_TRACE: setfullscreen resize mon='%s' target=%dx%d@%d,%d "
			"(c->mon->m=%dx%d@%d,%d)",
			c->mon->wlr_output->name,
			fsgeom.width, fsgeom.height, fsgeom.x, fsgeom.y,
			c->mon->m.width, c->mon->m.height, c->mon->m.x, c->mon->m.y);
		c->prev = c->geom;
		resize(c, fsgeom, 0);
		set_adaptive_sync(c->mon, 1);
		/* Reset frame tracking and video detection state */
		c->frame_time_idx = 0;
		c->frame_time_count = 0;
		c->detected_video_hz = 0.0f;
		c->last_buffer = NULL;
		c->video_detect_retries = 0;
		c->video_detect_phase = 0;
		/*
		 * Enable dynamic game VRR if this is a game.
		 * Game VRR will automatically adjust the display refresh rate
		 * to match the game's actual framerate for judder-free gaming.
		 */
		if (is_game_content(c) || client_wants_tearing(c)) {
			enable_game_vrr(c->mon);
		} else {
			/* Try to match video refresh rate if content is video */
			set_video_refresh_rate(c->mon, c);
		}
		/* Always run silent video detection - catches cutscenes in games */
		schedule_video_check(200);
	} else {
		/* restore previous size only for floating windows since their
		 * positions are set by the user. Tiled windows will be positioned
		 * by the layout system in arrange(). */
		if (c->isfloating)
			resize(c, c->prev, 0);
		set_adaptive_sync(c->mon, 0);
		/* Disable game VRR when exiting fullscreen */
		disable_game_vrr(c->mon);
		/* Restore max refresh rate when exiting fullscreen */
		restore_max_refresh_rate(c->mon);
		c->detected_video_hz = 0.0f;
		c->video_detect_phase = 0;
		c->video_detect_retries = 0;
		if (c->mon) {
			c->mon->frame_pacing_active = 0;
			c->mon->estimated_game_fps = 0.0f;
			c->mon->game_frame_interval_count = 0;
			c->mon->game_frame_interval_idx = 0;
			c->mon->frame_repeat_enabled = 0;
			c->mon->frame_repeat_count = 1;
			c->mon->frame_repeat_current = 0;
			c->mon->frame_repeat_candidate = 0;
			c->mon->frame_repeat_candidate_age = 0;
		}
	}
	arrange(c->mon);
	printstatus();
	/* Clear PID guard so game mode deactivates immediately */
	if (!fullscreen && c == game_mode_client)
		game_mode_pid = 0;
	update_game_mode();

	/* Ensure fullscreen client gets keyboard focus immediately */
	if (fullscreen) {
		exclusive_focus = NULL;  /* Clear any exclusive focus that might block */
		focusclient(c, 1);
	}
}

void
setmon(Client *c, Monitor *m, uint32_t newtags)
{
	Monitor *oldmon = c->mon;

	if (oldmon == m)
		return;
	wlr_log(WLR_INFO,
		"GAME_TRACE: setmon appid='%s' old='%s' → new='%s' "
		"isfullscreen=%d isfloating=%d geom=%dx%d@%d,%d newtags=0x%x",
		client_get_appid(c) ? client_get_appid(c) : "(null)",
		oldmon && oldmon->wlr_output ? oldmon->wlr_output->name : "(null)",
		m && m->wlr_output ? m->wlr_output->name : "(null)",
		c->isfullscreen, c->isfloating,
		c->geom.width, c->geom.height, c->geom.x, c->geom.y,
		newtags);
	c->mon = m;
	c->prev = c->geom;

	/* Scene graph sends surface leave/enter events on move and resize */
	if (oldmon)
		arrange(oldmon);
	if (m) {
		/* For floating windows, ensure they overlap with the new monitor.
		 * Tiled windows will be positioned by the layout system in arrange(). */
		if (c->isfloating)
			resize(c, c->geom, 0);
		c->tags = newtags ? newtags : m->tagset[m->seltags]; /* assign tags of target monitor */
		setfullscreen(c, c->isfullscreen); /* This will call arrange(c->mon) */
		setfloating(c, c->isfloating);
	}
	focusclient(focustop(selmon), 1);
}

void
setpsel(struct wl_listener *listener, void *data)
{
	/* This event is raised by the seat when a client wants to set the selection,
	 * usually when the user copies something. wlroots allows compositors to
	 * ignore such requests if they so choose, but in dwl we always honor them
	 */
	struct wlr_seat_request_set_primary_selection_event *event = data;
	wlr_seat_set_primary_selection(seat, event->source, event->serial);
}

void
setsel(struct wl_listener *listener, void *data)
{
	/* This event is raised by the seat when a client wants to set the selection,
	 * usually when the user copies something. wlroots allows compositors to
	 * ignore such requests if they so choose, but in dwl we always honor them
	 */
	struct wlr_seat_request_set_selection_event *event = data;
	wlr_seat_set_selection(seat, event->source, event->serial);
}

void
tag(const Arg *arg)
{
	Client *sel = focustop(selmon);
	if (!sel || (arg->ui & TAGMASK) == 0)
		return;

	sel->tags = arg->ui & TAGMASK;
	/* Switch to the new tag so we follow the window */
	selmon->tagset[selmon->seltags] = arg->ui & TAGMASK;
	focusclient(sel, 1);
	arrange(selmon);
	printstatus();
	/* Update game mode - fullscreen client visibility may have changed */
	update_game_mode();
}

void
tagmon(const Arg *arg)
{
	Client *sel = focustop(selmon);
	if (!sel)
		return;
	setmon(sel, dirtomon(arg->i), 0);
	free(sel->output);
	if (!(sel->output = strdup(sel->mon->wlr_output->name)))
		die("oom");
}

void
togglefloating(const Arg *arg)
{
	Client *sel = focustop(selmon);
	/* return if fullscreen */
	if (sel && !sel->isfullscreen)
		setfloating(sel, !sel->isfloating);
}

void
togglefullscreen(const Arg *arg)
{
	Client *sel = focustop(selmon);
	if (sel) {
		user_toggled_fullscreen = 1;
		setfullscreen(sel, !sel->isfullscreen);
		user_toggled_fullscreen = 0;
	}
}

void
togglefullscreenadaptivesync(const Arg *arg)
{
	fullscreen_adaptive_sync_enabled = !fullscreen_adaptive_sync_enabled;
}

void
toggletag(const Arg *arg)
{
	uint32_t newtags;
	Client *sel = focustop(selmon);
	if (!sel || !(newtags = sel->tags ^ (arg->ui & TAGMASK)))
		return;

	sel->tags = newtags;
	focusclient(focustop(selmon), 1);
	arrange(selmon);
	printstatus();
	/* Update game mode - fullscreen client visibility may have changed */
	update_game_mode();
}

void
toggleview(const Arg *arg)
{
	uint32_t newtagset;
	if (!(newtagset = selmon ? selmon->tagset[selmon->seltags] ^ (arg->ui & TAGMASK) : 0))
		return;

	selmon->tagset[selmon->seltags] = newtagset;
	focusclient(focustop(selmon), 1);
	arrange(selmon);
	printstatus();
	/* Update game mode when toggling tag visibility */
	update_game_mode();
}

/* Forward declarations for auto-kill */
static void read_proc_comm(pid_t pid, char *buf, size_t sz);
static int proc_autokill_eligible(const char *name);
static void schedule_autokill(const char *name);

void
unmapnotify(struct wl_listener *listener, void *data)
{
	/* Called when the surface is unmapped, and should no longer be shown. */
	Client *c = wl_container_of(listener, c, unmap);

	if (looks_like_game(c) || is_game_content(c))
		game_log("GAME_UNMAP: appid='%s' title='%s' type=%s "
			"was_fullscreen=%d steam_id=%d pid=%d",
			client_get_appid(c) ? client_get_appid(c) : "(null)",
			client_get_title(c) ? client_get_title(c) : "(null)",
			client_is_x11(c) ? "X11" : "XDG",
			c->isfullscreen, c->steam_game_id,
			(int)client_get_pid(c));

	if (c == grabc) {
		cursor_mode = CurNormal;
		grabc = NULL;
	}

	if (client_is_unmanaged(c)) {
		if (c == exclusive_focus) {
			exclusive_focus = NULL;
			focusclient(focustop(selmon), 1);
		}
	} else {
		wl_list_remove(&c->link);
		/* Remove from layout tree and rebalance BEFORE setmon calls arrange() */
		if (c->mon)
			remove_client(c->mon, c);
		setmon(c, NULL, 0);
		wl_list_remove(&c->flink);
	}
	/* Toggle adaptive sync off and restore refresh rate when fullscreen client is unmapped */
	if (c->isfullscreen) {
		Monitor *m = c->mon ? c->mon : selmon;
		set_adaptive_sync(m, 0);
		restore_max_refresh_rate(m);
	}

	/* Invalidate fullscreen classification cache */
	if (c->mon)
		c->mon->classify_cache_client = NULL;
	else if (selmon)
		selmon->classify_cache_client = NULL;

	if (c->foreign_toplevel_handle) {
		wlr_ext_foreign_toplevel_handle_v1_destroy(c->foreign_toplevel_handle);
		c->foreign_toplevel_handle = NULL;
	}

	wlr_scene_node_destroy(&c->scene->node);
	printstatus();
	motionnotify(0, NULL, 0, 0, 0, 0);

	/* Auto-kill: schedule background process cleanup */
	{
		pid_t akpid = client_get_pid(c);
		if (akpid > 1) {
			char akcomm[64];
			read_proc_comm(akpid, akcomm, sizeof(akcomm));
			normalize_proc_name(akcomm);
			if (akcomm[0] && proc_autokill_eligible(akcomm))
				schedule_autokill(akcomm);
		}
	}
	/* Clear PID guard so game mode deactivates immediately.
	 * Also match by PID in case game_mode_client was reassigned. */
	if (c == game_mode_client ||
	    (game_mode_pid > 1 && client_get_pid(c) == game_mode_pid)) {
		game_mode_pid = 0;
		game_mode_client = NULL;
	}
	update_game_mode();
}

void
updatetitle(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, set_title);
	if (c == focustop(c->mon))
		printstatus();
	if (c->foreign_toplevel_handle) {
		struct wlr_ext_foreign_toplevel_handle_v1_state ftstate = {
			.title = client_get_title(c),
			.app_id = client_get_appid(c),
		};
		wlr_ext_foreign_toplevel_handle_v1_update_state(
			c->foreign_toplevel_handle, &ftstate);
	}
}

void
urgent(struct wl_listener *listener, void *data)
{
	struct wlr_xdg_activation_v1_request_activate_event *event = data;
	Client *c = NULL;
	toplevel_from_wlr_surface(event->surface, &c, NULL);
	if (!c || c == focustop(selmon))
		return;

	c->isurgent = 1;
	printstatus();

	if (client_surface(c)->mapped)
		client_set_border_color(c, urgentcolor);
}

void
view(const Arg *arg)
{
	Client *c, *fullscreen_c = NULL;

	if (!selmon || (arg->ui & TAGMASK) == selmon->tagset[selmon->seltags])
		return;
	selmon->seltags ^= 1; /* toggle sel tagset */
	if (arg->ui & TAGMASK)
		selmon->tagset[selmon->seltags] = arg->ui & TAGMASK;

	/*
	 * Check if there's a fullscreen client on the new tag.
	 * If so, focus it directly instead of using focustop() which relies on
	 * the focus stack order. This fixes the bug where switching tags and back
	 * causes fullscreen games to lose keyboard focus.
	 */
	wl_list_for_each(c, &clients, link) {
		if (c->isfullscreen && VISIBLEON(c, selmon)) {
			fullscreen_c = c;
			break;
		}
	}

	if (fullscreen_c) {
		exclusive_focus = NULL;  /* Clear any exclusive focus that might block */
		focusclient(fullscreen_c, 1);
	} else {
		focusclient(focustop(selmon), 1);
	}

	arrange(selmon);
	printstatus();
	/* Update game mode when switching tags - a fullscreen game may become visible/hidden */
	update_game_mode();
	/* Update gamepad grab state - grab for HTPC views, release for Steam */
	gamepad_update_grab_state();
}

void
warpcursor(const Client *c)
{
	(void)c;
	/* Intentionally do not warp the cursor; keep it exactly where the user left it. */
}

void
zoom(const Arg *arg)
{
	Client *c, *sel = focustop(selmon);

	if (!sel || !selmon || !selmon->lt[selmon->sellt]->arrange || sel->isfloating)
		return;

	/* Search for the first tiled window that is not sel, marking sel as
	 * NULL if we pass it along the way */
	wl_list_for_each(c, &clients, link) {
		if (VISIBLEON(c, selmon) && !c->isfloating) {
			if (c != sel)
				break;
			sel = NULL;
		}
	}

	/* Return if no other tiled window was found */
	if (&c->link == &clients)
		return;

	/* If we passed sel, move c to the front; otherwise, move sel to the
	 * front */
	if (!sel)
		sel = c;
	wl_list_remove(&sel->link);
	wl_list_insert(&clients, &sel->link);

	focusclient(sel, 1);
	arrange(selmon);
}

void rotate_clients(const Arg *arg) {
	Monitor* m = selmon;
	Client *c;
	Client *first = NULL;
	Client *last = NULL;

	if (arg->i == 0)
		return;

	wl_list_for_each(c, &clients, link) {
		if (VISIBLEON(c, m) && !c->isfloating && !c->isfullscreen) {
			if (first == NULL) first = c;
			last = c;
		}
	}
	if (first != last) {
		struct wl_list *append_to = (arg->i > 0) ? &last->link : first->link.prev;
		struct wl_list *elem = (arg->i > 0) ? &first->link : &last->link;
		wl_list_remove(elem);
		wl_list_insert(append_to, elem);
		arrange(selmon);
	} 
}


void
togglesticky(const Arg *arg)
{
	Client *c = focustop(selmon);

	if (!c)
		return;

	setsticky(c, !c->issticky);
}

/* ── Auto-kill background processes ────────────────────────────────── */

typedef struct {
	char name[64];
	struct wl_event_source *timer;
} PendingAutoKill;

#define MAX_PENDING_AUTOKILLS 8
static PendingAutoKill pending_autokills[MAX_PENDING_AUTOKILLS];

static void
read_proc_comm(pid_t pid, char *buf, size_t sz)
{
	char path[64];
	FILE *f;
	size_t len;

	buf[0] = '\0';
	snprintf(path, sizeof(path), "/proc/%d/comm", (int)pid);
	f = fopen(path, "r");
	if (!f)
		return;
	if (fgets(buf, (int)sz, f)) {
		len = strlen(buf);
		if (len > 0 && buf[len - 1] == '\n')
			buf[len - 1] = '\0';
	}
	fclose(f);
}

static int
proc_autokill_eligible(const char *name)
{
	/* Apps that legitimately run without windows */
	static const char *bg_allowed[] = {
		/* Communication */
		"discord", "slack", "teams", "zoom", "skype", "telegram",
		"signal", "element", "fractal", "nheko", "thunderbird",
		"geary", "evolution",
		/* Audio players */
		"spotify", "rhythmbox", "clementine", "strawberry", "elisa",
		/* Game launchers */
		"steam", "lutris", "heroic", "bottles",
		/* Background services */
		"syncthing", "transmission", "qbittorrent", "deluge",
		"fragments",
		/* VMs */
		"virt-manager", "virtualbox", "vmware",
		/* Password managers */
		"keepassxc", "bitwarden", "1password",
	};

	/* User apps that SHOULD be killed (browsers, file managers, etc.) */
	static const char *kill_apps[] = {
		/* Browsers */
		"firefox", "chromium", "chrome", "brave", "vivaldi", "opera",
		"epiphany", "midori", "qutebrowser", "nyxt", "librewolf",
		"waterfox", "zen", "floorp", "thorium",
		/* File managers */
		"thunar", "nautilus", "dolphin", "nemo", "pcmanfm", "caja",
		"spacefm",
		/* Editors/IDEs */
		"code", "codium", "vscodium", "gedit", "kate", "sublime",
		"zed",
		/* Creative */
		"blender", "gimp", "inkscape", "krita", "darktable",
		"rawtherapee", "kdenlive", "shotcut", "openshot", "obs",
		"audacity", "ardour", "lmms", "bitwig", "reaper", "godot",
		/* Office */
		"libreoffice", "soffice", "writer", "calc", "impress",
		"draw", "onlyoffice", "wps", "evince", "okular", "zathura",
		"mupdf",
		/* Media players */
		"vlc", "celluloid", "totem", "parole", "smplayer",
		/* Misc */
		"calibre", "anki", "logseq", "obsidian", "joplin", "drawio",
		"postman", "insomnia", "dbeaver", "pgadmin",
	};
	size_t i;

	if (!name || !*name)
		return 0;

	/* Check bg_allowed first — these should NOT be killed */
	for (i = 0; i < LENGTH(bg_allowed); i++) {
		if (strcasestr(name, bg_allowed[i]))
			return 0;
	}

	/* Check if it's a user app that should be killed */
	for (i = 0; i < LENGTH(kill_apps); i++) {
		if (strcasestr(name, kill_apps[i]))
			return 1;
	}

	return 0;
}

static int
autokill_timer_cb(void *data)
{
	PendingAutoKill *ak = data;
	Client *c;
	char comm[64];
	DIR *d;
	struct dirent *de;

	/* Check if any client still has this process name */
	wl_list_for_each(c, &clients, link) {
		pid_t pid = client_get_pid(c);
		if (pid <= 1)
			continue;
		read_proc_comm(pid, comm, sizeof(comm));
		normalize_proc_name(comm);
		if (strcasecmp(comm, ak->name) == 0) {
			/* Process still has windows — cancel */
			goto cleanup;
		}
	}

	/* No windows remain — SIGTERM all processes with this name */
	d = opendir("/proc");
	if (d) {
		while ((de = readdir(d)) != NULL) {
			pid_t pid;
			if (de->d_name[0] < '1' || de->d_name[0] > '9')
				continue;
			pid = (pid_t)atoi(de->d_name);
			if (pid <= 1)
				continue;
			read_proc_comm(pid, comm, sizeof(comm));
			normalize_proc_name(comm);
			if (strcasecmp(comm, ak->name) == 0)
				kill(pid, SIGTERM);
		}
		closedir(d);
	}

cleanup:
	wl_event_source_remove(ak->timer);
	ak->timer = NULL;
	ak->name[0] = '\0';
	return 0;
}

static void
schedule_autokill(const char *name)
{
	int i;
	PendingAutoKill *slot = NULL;

	/* Check for duplicate pending */
	for (i = 0; i < MAX_PENDING_AUTOKILLS; i++) {
		if (pending_autokills[i].timer &&
		    strcasecmp(pending_autokills[i].name, name) == 0)
			return; /* already pending */
	}

	/* Find free slot */
	for (i = 0; i < MAX_PENDING_AUTOKILLS; i++) {
		if (!pending_autokills[i].timer) {
			slot = &pending_autokills[i];
			break;
		}
	}
	if (!slot)
		return; /* all slots busy */

	snprintf(slot->name, sizeof(slot->name), "%s", name);
	slot->timer = wl_event_loop_add_timer(event_loop, autokill_timer_cb, slot);
	if (slot->timer)
		wl_event_source_timer_update(slot->timer, 2000); /* 2 seconds */
	else
		slot->name[0] = '\0';
}
