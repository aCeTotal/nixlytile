#include "nixlytile.h"
#include "client.h"

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

	appid = client_get_appid(c);
	title = client_get_title(c);

	for (r = rules; r < rules + nrules; r++) {
		if ((!r->title || strstr(title, r->title))
				&& (!r->id || strstr(appid, r->id))) {
			c->isfloating = r->isfloating;
			newtags |= r->tags;
			i = 0;
			wl_list_for_each(m, &mons, link) {
				if (r->monitor == i++)
					mon = m;
			}
		}
	}

	if (mon) {
		c->geom.x = (mon->w.width - c->geom.width) / 2 + mon->m.x;
		c->geom.y = (mon->w.height - c->geom.height) / 2 + mon->m.y;
	}

	c->isfloating |= client_is_float_type(c);
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
	} else
#endif
	{
		wl_list_remove(&c->commit.link);
		wl_list_remove(&c->map.link);
		wl_list_remove(&c->unmap.link);
		wl_list_remove(&c->maximize.link);
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
			client_set_border_color(old_c, bordercolor);

			client_activate_surface(old, 0);
		}
	}
	printstatus();

	if (!c) {
		/* With no client, all we have left is to clear focus */
		wlr_seat_keyboard_notify_clear_focus(seat);
		/* Notify text inputs about focus loss */
		text_input_focus_change(old, NULL);
		/* Hide cursor if HTPC views are visible */
		if (selmon && (selmon->pc_gaming.visible ||
		               selmon->movies_view.visible ||
		               selmon->tvshows_view.visible))
			wlr_cursor_set_surface(cursor, NULL, 0, 0);
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
	setfullscreen(c, client_wants_fullscreen(c));
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
	c->scene = client_surface(c)->data = wlr_scene_tree_create(layers[LyrTile]);
	/* Enabled later by a call to arrange() */
	wlr_scene_node_set_enabled(&c->scene->node, client_is_unmanaged(c));
	c->scene_surface = c->type == XDGShell
			? wlr_scene_xdg_surface_create(c->scene, c->surface.xdg)
			: wlr_scene_subsurface_tree_create(c->scene, client_surface(c));
	c->scene->node.data = c->scene_surface->node.data = c;

	client_get_geometry(c, &c->geom);

	/* Handle unmanaged clients first so we can return prior create borders */
	if (client_is_unmanaged(c)) {
		/* Unmanaged clients always are floating */
		wlr_scene_node_reparent(&c->scene->node, layers[LyrFloat]);
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

	/* Set initial monitor, tags, floating status, and focus:
	 * we always consider floating, clients that have parent and thus
	 * we set the same tags and monitor as its parent.
	 * If there is no parent, apply rules */
	if ((p = client_get_parent(c))) {
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
		applyrules(c);
		if (tray_anchor_time_ms && monotonic_msec() - tray_anchor_time_ms <= 1000) {
			Monitor *am = xytomon(tray_anchor_x, tray_anchor_y);
			if (!am)
				am = selmon;
			if (am) {
				c->isfloating = 1;
				c->geom.x = tray_anchor_x - c->geom.width / 2;
				c->geom.y = tray_anchor_y;
				setmon(c, am, c->tags);
			}
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
			wlr_cursor_set_xcursor(cursor, cursor_mgr, "default");
		}
	}

	/*
	 * AUTO-FULLSCREEN FOR GAMES
	 *
	 * Steam games often start in windowed mode and then request fullscreen
	 * after a delay (sometimes several seconds). This causes an annoying
	 * flash where the game appears windowed before going fullscreen.
	 *
	 * To fix this, we detect games at map time and immediately set them
	 * to fullscreen, skipping the windowed phase entirely.
	 *
	 * Detection criteria:
	 * - Client is a child process of Steam (game launcher)
	 * - Client uses content-type=game protocol hint
	 * - Client requests tearing (low-latency mode used by games)
	 */
	if (!c->isfloating && !c->isfullscreen && looks_like_game(c)) {
		wlr_log(WLR_INFO, "Auto-fullscreen: detected game '%s', setting fullscreen immediately",
			client_get_appid(c) ? client_get_appid(c) : "(unknown)");
		setfullscreen(c, 1);
		/* Immediately focus the game */
		exclusive_focus = NULL;
		focusclient(c, 1);
		/*
		 * Schedule a delayed refocus for XWayland games.
		 * Wine/Proton games often need extra time after mapping before
		 * they properly accept keyboard input. Without this, users
		 * can't skip intros until they switch tags and back.
		 */
#ifdef XWAYLAND
		if (c->type == X11)
			schedule_game_refocus(c, 150);
#endif
		printstatus();
		return; /* Skip unset_fullscreen logic - we just set fullscreen */
	}

	printstatus();

unset_fullscreen:
	m = c->mon ? c->mon : xytomon(c->geom.x, c->geom.y);
	/* In HTPC mode, don't unset fullscreen for Steam popups appearing */
	if (htpc_mode_active && (is_steam_popup(c) || (p && is_steam_client(p))))
		return;
	wl_list_for_each(w, &clients, link) {
		if (w != c && w != p && w->isfullscreen && m == w->mon && (w->tags & c->tags))
			setfullscreen(w, 0);
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
	Client *p = client_get_parent(c);
	c->isfloating = floating;
	/* If in floating layout do not change the client's layer */
	if (!c->mon || !client_surface(c)->mapped || !c->mon->lt[c->mon->sellt]->arrange)
		return;
	wlr_scene_node_reparent(&c->scene->node, layers[c->isfullscreen ||
			(p && p->isfullscreen) ? LyrFS
			: c->isfloating ? LyrFloat : LyrTile]);
	arrange(c->mon);
	printstatus();
}

void
setfullscreen(Client *c, int fullscreen)
{
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

	c->isfullscreen = fullscreen;
	if (!c->mon || !client_surface(c)->mapped)
		return;
	c->bw = fullscreen ? 0 : borderpx;
	client_set_fullscreen(c, fullscreen);
	wlr_scene_node_reparent(&c->scene->node, layers[c->isfullscreen
			? LyrFS : c->isfloating ? LyrFloat : LyrTile]);

	if (fullscreen) {
		c->prev = c->geom;
		resize(c, c->mon->m, 0);
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
	}
	arrange(c->mon);
	printstatus();
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
	if (sel)
		setfullscreen(sel, !sel->isfullscreen);
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

void
unmapnotify(struct wl_listener *listener, void *data)
{
	/* Called when the surface is unmapped, and should no longer be shown. */
	Client *c = wl_container_of(listener, c, unmap);
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

	wlr_scene_node_destroy(&c->scene->node);
	printstatus();
	motionnotify(0, NULL, 0, 0, 0, 0);
	update_game_mode();
}

void
updatetitle(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, set_title);
	if (c == focustop(c->mon))
		printstatus();
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
