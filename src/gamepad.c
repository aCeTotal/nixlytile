#include "nixlytile.h"
#include "client.h"

void
gamepad_menu_show(Monitor *m)
{
	GamepadMenu *gm;
	int w, h, x, y;
	/* Near-opaque dark overlay to obscure background (simulates heavy blur) */
	float dim_color[4] = {0.08f, 0.08f, 0.10f, 0.97f};

	if (!m)
		return;

	gamepad_menu_hide_all();

	gm = &m->gamepad_menu;
	gm->selected = 0;
	htpc_menu_build();  /* Build menu based on enabled pages */
	gm->item_count = htpc_menu_item_count;

	/* Calculate popup size - just buttons, no title/hints */
	w = 300;
	/* Add extra separator space between gaming and streaming sections */
	int separator_gap = 20;
	h = 20 + htpc_menu_item_count * 55 + separator_gap + 20;

	/* Center on monitor */
	x = m->m.x + (m->m.width - w) / 2;
	y = m->m.y + (m->m.height - h) / 2;

	gm->x = x;
	gm->y = y;
	gm->width = w;
	gm->height = h;
	gm->visible = 1;

	/* Create dim overlay to obscure background */
	if (!gm->dim)
		gm->dim = wlr_scene_tree_create(layers[LyrBlock]);
	if (gm->dim) {
		struct wlr_scene_node *node, *tmp;
		wl_list_for_each_safe(node, tmp, &gm->dim->children, link)
			wlr_scene_node_destroy(node);
		wlr_scene_node_set_position(&gm->dim->node, m->m.x, m->m.y);
		drawrect(gm->dim, 0, 0, m->m.width, m->m.height, dim_color);
		wlr_scene_node_set_enabled(&gm->dim->node, 1);
		wlr_scene_node_raise_to_top(&gm->dim->node);
	}

	if (!gm->tree)
		gm->tree = wlr_scene_tree_create(layers[LyrBlock]);
	if (!gm->tree)
		return;

	wlr_scene_node_set_position(&gm->tree->node, x, y);
	wlr_scene_node_set_enabled(&gm->tree->node, 1);
	wlr_scene_node_raise_to_top(&gm->tree->node);

	gamepad_menu_render(m);
}

void
gamepad_menu_hide(Monitor *m)
{
	GamepadMenu *gm;

	if (!m)
		return;

	gm = &m->gamepad_menu;
	if (!gm->visible)
		return;

	gm->visible = 0;
	if (gm->tree)
		wlr_scene_node_set_enabled(&gm->tree->node, 0);
	if (gm->dim)
		wlr_scene_node_set_enabled(&gm->dim->node, 0);
}

void
gamepad_menu_hide_all(void)
{
	Monitor *m;

	wl_list_for_each(m, &mons, link) {
		gamepad_menu_hide(m);
	}
}

Monitor *
gamepad_menu_visible_monitor(void)
{
	Monitor *m;

	wl_list_for_each(m, &mons, link) {
		if (m->gamepad_menu.visible)
			return m;
	}
	return NULL;
}

void
gamepad_menu_render(Monitor *m)
{
	GamepadMenu *gm;
	struct wlr_scene_node *node, *tmp;
	int padding = 15;
	int item_height = 55;
	int button_margin = 10;
	int y_offset;
	float bg_color[4] = {0.12f, 0.12f, 0.14f, 0.95f};
	float button_color[4] = {0.18f, 0.18f, 0.22f, 1.0f};
	float selected_color[4] = {0.25f, 0.5f, 0.9f, 1.0f};
	float border_color[4] = {0.35f, 0.35f, 0.4f, 1.0f};

	if (!m || !statusfont.font)
		return;

	gm = &m->gamepad_menu;
	if (!gm->visible || !gm->tree)
		return;

	/* Clear previous content */
	wl_list_for_each_safe(node, tmp, &gm->tree->children, link) {
		if (gm->bg && node == &gm->bg->node)
			continue;
		wlr_scene_node_destroy(node);
	}

	/* Create/update background */
	if (!gm->bg)
		gm->bg = wlr_scene_tree_create(gm->tree);
	if (gm->bg) {
		wl_list_for_each_safe(node, tmp, &gm->bg->children, link)
			wlr_scene_node_destroy(node);
		wlr_scene_node_set_position(&gm->bg->node, 0, 0);
		drawrect(gm->bg, 0, 0, gm->width, gm->height, bg_color);
		/* Outer border */
		draw_border(gm->bg, 0, 0, gm->width, gm->height, 2, border_color);
	}

	/* Draw menu items as buttons with equal spacing */
	int current_y = padding;
	for (int i = 0; i < gm->item_count; i++) {
		const char *label = htpc_menu_items[i].label;
		int button_x = padding;
		int button_w = gm->width - 2 * padding;
		int button_h = item_height - button_margin;
		int label_w = status_text_width(label);
		/* Center label horizontally within button */
		int label_x = (button_w - label_w) / 2;
		int button_y = current_y;

		/* Draw button background */
		if (i == gm->selected) {
			drawrect(gm->tree, button_x, button_y, button_w, button_h, selected_color);
		} else {
			drawrect(gm->tree, button_x, button_y, button_w, button_h, button_color);
		}

		/* Button border */
		float btn_border[4] = {0.4f, 0.4f, 0.45f, 1.0f};
		draw_border(gm->tree, button_x, button_y, button_w, button_h, 1, btn_border);

		/* Button label - create subtree positioned at button location */
		struct wlr_scene_tree *label_tree = wlr_scene_tree_create(gm->tree);
		if (label_tree) {
			wlr_scene_node_set_position(&label_tree->node, button_x, button_y);
			StatusModule mod = {0};
			mod.tree = label_tree;
			/* tray_render_label centers text vertically within bar_height */
			tray_render_label(&mod, label, label_x, button_h, statusbar_fg);
		}

		current_y += item_height;

		/* Add extra separator gap after PC-gaming (before streaming services) */
		if (strcmp(label, "PC-gaming") == 0)
			current_y += 20;
	}
}

int
gamepad_menu_handle_click(Monitor *m, int cx, int cy, uint32_t button)
{
	GamepadMenu *gm;
	int padding = 15;
	int item_height = 55;
	int relx, rely;
	int clicked_item;

	if (!m)
		return 0;

	gm = &m->gamepad_menu;
	if (!gm->visible)
		return 0;

	/* Check if click is inside menu bounds */
	relx = cx - gm->x;
	rely = cy - gm->y;

	if (relx < 0 || rely < 0 || relx >= gm->width || rely >= gm->height) {
		/* Click outside menu - close it */
		gamepad_menu_hide(m);
		return 1;
	}

	/* Only handle left click for selection */
	if (button != BTN_LEFT)
		return 1;

	/* Calculate which item was clicked - account for separator gap */
	clicked_item = -1;
	int current_y = padding;
	for (int i = 0; i < gm->item_count; i++) {
		int button_h = item_height - 10; /* button_margin */
		if (rely >= current_y && rely < current_y + button_h) {
			clicked_item = i;
			break;
		}
		current_y += item_height;
		/* Account for separator after PC-gaming */
		if (strcmp(htpc_menu_items[i].label, "PC-gaming") == 0)
			current_y += 20;
	}

	if (clicked_item >= 0 && clicked_item < gm->item_count) {
		gm->selected = clicked_item;
		gamepad_menu_select(m);
	}

	return 1;
}

void
gamepad_menu_select(Monitor *m)
{
	GamepadMenu *gm;
	const char *cmd;
	const char *label;

	if (!m)
		return;

	gm = &m->gamepad_menu;
	if (!gm->visible || gm->selected < 0 || gm->selected >= gm->item_count)
		return;

	label = htpc_menu_items[gm->selected].label;
	cmd = htpc_menu_items[gm->selected].command;

	/* Handle PC-gaming - launch Steam Big Picture only */
	if (strcmp(label, "PC-gaming") == 0) {
		gamepad_menu_hide_all();
		wlr_log(WLR_INFO, "Launching Steam Big Picture");

		/* Launch Steam Big Picture if not already running */
		steam_launch_bigpicture();
		return;
	}

	/* Handle Retro-gaming - switch to tag 3 and show retro console selection */
	if (strcmp(label, "Retro-gaming") == 0) {
		gamepad_menu_hide_all();
		steam_kill();
		wlr_log(WLR_INFO, "Switching to Retro Gaming (tag 3)");

		/* Switch to tag 3 */
		if (selmon) {
			selmon->seltags ^= 1;
			selmon->tagset[selmon->seltags] = 1 << 2; /* Tag 3 = bit 2 */
			focusclient(focustop(selmon), 1);
			arrange(selmon);
			printstatus();
		}

		retro_gaming_show(m);
		return;
	}

	/* Handle Movies - switch to tag 2 and show movies grid view */
	if (strcmp(label, "Movies") == 0) {
		gamepad_menu_hide_all();
		steam_kill();
		live_tv_kill();
		wlr_log(WLR_INFO, "Switching to Movies (tag 2)");

		/* Switch to tag 2 - use m consistently (same monitor as menu) */
		m->seltags ^= 1;
		m->tagset[m->seltags] = 1 << 1; /* Tag 2 = bit 1 */

		/* Show view BEFORE arrange so htpc_views_update_visibility sees correct state */
		media_view_show(m, MEDIA_VIEW_MOVIES);

		focusclient(focustop(m), 1);
		arrange(m);
		printstatus();
		return;
	}

	/* Handle TV-shows - switch to tag 1 and show tvshows grid view */
	if (strcmp(label, "TV-shows") == 0) {
		gamepad_menu_hide_all();
		steam_kill();
		live_tv_kill();
		wlr_log(WLR_INFO, "Switching to TV-shows (tag 1)");

		/* Switch to tag 1 - use m consistently (same monitor as menu) */
		m->seltags ^= 1;
		m->tagset[m->seltags] = 1 << 0; /* Tag 1 = bit 0 */

		/* Show view BEFORE arrange so htpc_views_update_visibility sees correct state */
		media_view_show(m, MEDIA_VIEW_TVSHOWS);

		focusclient(focustop(m), 1);
		arrange(m);
		printstatus();
		return;
	}

	/* Handle streaming services via Chromium kiosk */
	{
		static const struct { const char *label; const char *url; } kiosk_services[] = {
			{"NRK",      NRK_URL},
			{"Netflix",  NETFLIX_URL},
			{"Viaplay",  VIAPLAY_URL},
			{"TV2 Play", TV2PLAY_URL},
			{"F1TV",     F1TV_URL},
		};

		for (size_t i = 0; i < LENGTH(kiosk_services); i++) {
			if (strcmp(label, kiosk_services[i].label) == 0) {
				gamepad_menu_hide_all();
				steam_kill();
				live_tv_kill();
				media_view_hide_all();
				retro_gaming_hide_all();
				pc_gaming_hide_all();
				wlr_log(WLR_INFO, "Launching %s in Chromium kiosk", kiosk_services[i].label);
				pid_t pid = fork();
				if (pid == 0) {
					setsid();
					execlp("chromium", "chromium",
						"--ozone-platform=wayland",
						"--kiosk", "--start-fullscreen",
						"--autoplay-policy=no-user-gesture-required",
						"--enable-features=VaapiVideoDecoder,PlatformHEVCDecoderSupport",
						"--disable-gpu-vsync",
						"--disable-frame-rate-limit",
						"--force-device-scale-factor=1",
						"--disable-translate",
						kiosk_services[i].url, (char *)NULL);
					_exit(127);
				}
				return;
			}
		}
	}

	/* Handle Quit HTPC - shutdown system (htpc-only has no desktop to return to) */
	if (strcmp(label, "Quit HTPC") == 0) {
		gamepad_menu_hide_all();
		steam_kill();
		wlr_log(WLR_INFO, "Quit HTPC: shutting down");
		quit(NULL);
		return;
	}

	/* Execute command if set, otherwise just log the selection */
	if (cmd && cmd[0]) {
		Arg arg = {.v = cmd};
		spawn(&arg);
	} else {
		wlr_log(WLR_INFO, "Gamepad menu selected: %s", label);
	}

	gamepad_menu_hide_all();
}

int
gamepad_menu_handle_button(Monitor *m, int button, int value)
{
	GamepadMenu *gm;

	if (!m)
		return 0;

	/* Handle on-screen keyboard FIRST - it has highest priority when visible
	 * Check all monitors since OSK might be on a different monitor */
	{
		Monitor *osk_mon = osk_visible_monitor();
		if (osk_mon) {
			if (osk_handle_button(osk_mon, button, value))
				return 1;
		}
	}

	/* Handle wifi popup with gamepad (A = connect, B = cancel) */
	if (m->wifi_popup.visible && value == 1) {
		switch (button) {
		case BTN_SOUTH:  /* A button - connect */
			if (!m->wifi_popup.connecting && m->wifi_popup.password_len > 0)
				wifi_popup_connect(m);
			return 1;
		case BTN_EAST:   /* B button - cancel */
			wifi_popup_hide_all();
			return 1;
		}
		return 1;  /* Consume other buttons when popup is open */
	}

	/* Handle sudo popup with gamepad (A = submit, B = cancel) */
	if (m->sudo_popup.visible && value == 1) {
		switch (button) {
		case BTN_SOUTH:  /* A button - submit */
			if (!m->sudo_popup.running && m->sudo_popup.password_len > 0)
				sudo_popup_execute(m);
			return 1;
		case BTN_EAST:   /* B button - cancel */
			sudo_popup_hide_all();
			return 1;
		}
		return 1;  /* Consume other buttons when popup is open */
	}

	gm = &m->gamepad_menu;

	/* BTN_MODE (guide button) - instant press to toggle menu */
	if (button == BTN_MODE && value == 1) {
		if (gm->visible)
			gamepad_menu_hide(m);
		else
			gamepad_menu_show(m);
		return 1;  /* Consume the event */
	}

	/* Menu navigation takes priority when menu is visible (on button press only) */
	if (gm->visible && value == 1) {
		switch (button) {
		case BTN_SOUTH:  /* A button - select */
			gamepad_menu_select(m);
			return 1;
		case BTN_EAST:   /* B button - close */
			gamepad_menu_hide(m);
			return 1;
		case BTN_DPAD_UP:
			if (gm->selected > 0) {
				gm->selected--;
				gamepad_menu_render(m);
			}
			return 1;
		case BTN_DPAD_DOWN:
			if (gm->selected < gm->item_count - 1) {
				gm->selected++;
				gamepad_menu_render(m);
			}
			return 1;
		}
	}

	/* Check if PC gaming view is active on ANY monitor
	 * Use pc_gaming_visible_monitor() to find the correct monitor since
	 * selmon may have changed due to mouse movement */
	{
		Monitor *pg_mon = pc_gaming_visible_monitor();
		if (pg_mon && htpc_view_is_active(pg_mon, pg_mon->pc_gaming.view_tag, pg_mon->pc_gaming.visible)) {
			if (pc_gaming_handle_button(pg_mon, button, value))
				return 1;
			/* Block guide button when install popup is visible */
			if (pg_mon->pc_gaming.install_popup_visible && button == BTN_MODE)
				return 0;  /* Let Steam handle it */
			/* Fall through to mouse click emulation if a popup client has focus */
		}
	}

	/* Check if Retro gaming view is active on ANY monitor */
	{
		Monitor *rg_mon = retro_gaming_visible_monitor();
		if (rg_mon && htpc_view_is_active(rg_mon, rg_mon->retro_gaming.view_tag, rg_mon->retro_gaming.visible)) {
			if (retro_gaming_handle_button(rg_mon, button, value))
				return 1;
		}
	}

	/* Handle integrated video player controls first (highest priority when playing) */
	if (active_videoplayer && playback_state == PLAYBACK_PLAYING) {
		if (value == 1) {
			if (handle_playback_osd_input(button))
				return 1;
		} else if (value == 0) {
			/* Button release - stop hold-to-seek for shoulder buttons */
			if ((button == BTN_TL || button == BTN_TR) &&
			    active_videoplayer->control_bar.seek_hold_active) {
				videoplayer_seek_hold_stop(active_videoplayer);
				render_playback_osd();
				return 1;
			}
		}
	}

	/* Check if Movies or TV-shows view is active on ANY monitor
	 * Use media_view_visible_monitor() to find the correct monitor since
	 * selmon may have changed due to mouse movement */
	{
		Monitor *media_mon = media_view_visible_monitor();
		if (media_mon) {
			if (htpc_view_is_active(media_mon, media_mon->movies_view.view_tag, media_mon->movies_view.visible)) {
				if (media_view_handle_button(media_mon, MEDIA_VIEW_MOVIES, button, value))
					return 1;
			}
			if (htpc_view_is_active(media_mon, media_mon->tvshows_view.view_tag, media_mon->tvshows_view.visible)) {
				if (media_view_handle_button(media_mon, MEDIA_VIEW_TVSHOWS, button, value))
					return 1;
			}
		}
	}

	/* Mouse click emulation with shoulder buttons (when menu is not visible) */
	if (!gm->visible) {
		/* Skip click emulation if a fullscreen client has focus (let game handle it)
		 * Exception: In HTPC mode, allow click emulation for browsers */
		Client *focused = focustop(selmon);
		if (focused && focused->isfullscreen &&
		    !(htpc_mode_active && is_browser_client(focused)))
			return 0;

		/* Y button (BTN_NORTH) - toggle on-screen keyboard in HTPC mode */
		if (button == BTN_NORTH && value == 1 && htpc_mode_active) {
			Monitor *osk_mon = osk_visible_monitor();
			if (osk_mon) {
				osk_hide(osk_mon);
			} else if (selmon) {
				osk_show(selmon, NULL);
			}
			return 1;
		}

		/* X button (BTN_WEST) - show OSK when in browser (for text input) */
		if (button == BTN_WEST && value == 1 && htpc_mode_active) {
			if (focused && is_browser_client(focused)) {
				Monitor *osk_mon = osk_visible_monitor();
				if (!osk_mon && selmon) {
					osk_show(selmon, NULL);
				}
				return 1;
			}
		}

		uint32_t time_msec = (uint32_t)(monotonic_msec() & 0xFFFFFFFF);
		uint32_t mapped_button = 0;
		uint32_t state = value ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED;

		switch (button) {
		case BTN_TR:  /* Right bumper = left click */
		case BTN_SOUTH:  /* A button = left click */
			mapped_button = BTN_LEFT;
			break;
		case BTN_TL:  /* Left bumper = right click */
		case BTN_EAST:  /* B button = right click */
			mapped_button = BTN_RIGHT;
			break;
		case BTN_THUMBL:  /* Left stick click = middle click */
			mapped_button = BTN_MIDDLE;
			break;
		}

		if (mapped_button) {
			/* Use internal handler for full statusbar/tile support */
			handle_pointer_button_internal(mapped_button, state, time_msec);
			return 1;
		}
		return 0;
	}

	return 0;
}

int
gamepad_is_gamepad_device(int fd)
{
	unsigned long evbit[((EV_MAX) / (8 * sizeof(unsigned long))) + 1] = {0};
	unsigned long keybit[((KEY_MAX) / (8 * sizeof(unsigned long))) + 1] = {0};

	/* Get event types */
	if (ioctl(fd, EVIOCGBIT(0, sizeof(evbit)), evbit) < 0)
		return 0;

	/* Must have EV_KEY */
	if (!(evbit[0] & (1 << EV_KEY)))
		return 0;

	/* Get key capabilities */
	if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybit)), keybit) < 0)
		return 0;

	/* Check for gamepad-specific buttons */
	/* BTN_GAMEPAD is 0x130, BTN_SOUTH is 0x130, BTN_MODE is 0x13c */
	int has_gamepad_btn = 0;

	/* Check for BTN_SOUTH (A button) - 0x130 = 304 */
	if (keybit[BTN_SOUTH / (8 * sizeof(unsigned long))] & (1UL << (BTN_SOUTH % (8 * sizeof(unsigned long)))))
		has_gamepad_btn = 1;

	/* Check for BTN_MODE (guide button) - 0x13c = 316 */
	if (keybit[BTN_MODE / (8 * sizeof(unsigned long))] & (1UL << (BTN_MODE % (8 * sizeof(unsigned long)))))
		has_gamepad_btn = 1;

	/* Reject if it has typical keyboard keys (KEY_A through KEY_Z) */
	/* KEY_A = 30 */
	if (keybit[KEY_A / (8 * sizeof(unsigned long))] & (1UL << (KEY_A % (8 * sizeof(unsigned long)))))
		return 0;

	/* Reject if it has BTN_LEFT (mouse) - 0x110 = 272 */
	if (keybit[BTN_LEFT / (8 * sizeof(unsigned long))] & (1UL << (BTN_LEFT % (8 * sizeof(unsigned long)))))
		return 0;

	return has_gamepad_btn;
}

int
gamepad_event_cb(int fd, uint32_t mask, void *data)
{
	GamepadDevice *gp = data;
	GamepadDevice *iter;
	struct input_event ev;
	Monitor *m;
	ssize_t n;
	int found = 0;

	if (!gp)
		return 0;

	/* Verify gp is still in the list (not already freed by inotify handler) */
	wl_list_for_each(iter, &gamepads, link) {
		if (iter == gp) {
			found = 1;
			break;
		}
	}
	if (!found)
		return 0;

	/* Handle device disconnection/error */
	if (mask & (WL_EVENT_ERROR | WL_EVENT_HANGUP)) {
		wlr_log(WLR_INFO, "Gamepad disconnected (fd event): %s (%s)", gp->name, gp->path);
		if (gp->event_source) {
			wl_event_source_remove(gp->event_source);
			gp->event_source = NULL;
		}
		if (gp->fd >= 0) {
			close(gp->fd);
			gp->fd = -1;
		}
		wl_list_remove(&gp->link);
		free(gp);
		return 0;
	}

	while ((n = read(fd, &ev, sizeof(ev))) == sizeof(ev)) {
		/* Skip sync events for activity tracking */
		if (ev.type == EV_SYN)
			continue;

		/* Update activity timestamp */
		gp->last_activity_ms = monotonic_msec();

		/* Handle wake from suspended state - guide button wakes the controller */
		if (gp->suspended) {
			if (ev.type == EV_KEY && ev.code == BTN_MODE && ev.value == 1) {
				gamepad_resume(gp);
			}
			continue;  /* Ignore other input while suspended */
		}

		/* Ignore button presses during connect grace period (500ms).
		 * Bluetooth controllers often send synthetic events on reconnect
		 * (e.g., BTN_EAST) that would inadvertently stop video playback. */
		if (gp->connect_time_ms && ev.type == EV_KEY && ev.value == 1 &&
		    monotonic_msec() - gp->connect_time_ms < 500) {
			wlr_log(WLR_DEBUG, "Ignoring synthetic button during connect grace: %d",
				ev.code);
			continue;
		}

		/* Find the currently selected monitor */
		m = selmon ? selmon : (wl_list_empty(&mons) ? NULL :
			wl_container_of(mons.next, m, link));

		/* Handle key/button events */
		if (ev.type == EV_KEY) {
			if (m)
				gamepad_menu_handle_button(m, ev.code, ev.value);
			continue;
		}

		/* Handle axis events */
		if (ev.type == EV_ABS) {
			/* Ignore synthetic D-pad events during connect grace period (500ms).
			 * Bluetooth controllers may send ABS_HAT0X/Y on reconnect. */
			if ((ev.code == ABS_HAT0X || ev.code == ABS_HAT0Y) &&
			    gp->connect_time_ms && monotonic_msec() - gp->connect_time_ms < 500)
				continue;

			/* Joystick axes for cursor movement */
			switch (ev.code) {
			case ABS_X:
				gp->left_x = ev.value;
				break;
			case ABS_Y:
				gp->left_y = ev.value;
				break;
			case ABS_RX:
				gp->right_x = ev.value;
				break;
			case ABS_RY:
				gp->right_y = ev.value;
				break;
			}

			/* Show playback OSD on stick movement during video playback */
			if (active_videoplayer && playback_state == PLAYBACK_PLAYING &&
			    (ev.code == ABS_X || ev.code == ABS_Y ||
			     ev.code == ABS_RX || ev.code == ABS_RY)) {
				int offset = 0, range = 32767;
				switch (ev.code) {
				case ABS_X:
					offset = gp->left_x - gp->cal_lx.center;
					range = (gp->cal_lx.max - gp->cal_lx.min) / 2;
					break;
				case ABS_Y:
					offset = gp->left_y - gp->cal_ly.center;
					range = (gp->cal_ly.max - gp->cal_ly.min) / 2;
					break;
				case ABS_RX:
					offset = gp->right_x - gp->cal_rx.center;
					range = (gp->cal_rx.max - gp->cal_rx.min) / 2;
					break;
				case ABS_RY:
					offset = gp->right_y - gp->cal_ry.center;
					range = (gp->cal_ry.max - gp->cal_ry.min) / 2;
					break;
				}
				if (range == 0) range = 32767;
				int deadzone = range * GAMEPAD_DEADZONE / 32767;
				if (abs(offset) > deadzone)
					render_playback_osd();
			}

			switch (ev.code) {
			case ABS_HAT0X:
				/* D-pad left/right for navigation */
				if (ev.value != 0) {
					int handled = 0;
					int button = ev.value < 0 ? BTN_DPAD_LEFT : BTN_DPAD_RIGHT;

					/* Check OSK first - highest priority */
					Monitor *osk_mon = osk_visible_monitor();
					if (osk_mon) {
						handled = osk_handle_button(osk_mon, button, 1);
						/* Start repeat timer for hold-to-slide */
						osk_dpad_repeat_start(osk_mon, button);
					}

					/* Check Retro gaming on any monitor (must be active on current tag) */
					Monitor *rg_mon = retro_gaming_visible_monitor();
					if (rg_mon && htpc_view_is_active(rg_mon, rg_mon->retro_gaming.view_tag, rg_mon->retro_gaming.visible)) {
						handled = retro_gaming_handle_button(rg_mon, button, 1);
					}

					/* Check PC gaming on any monitor */
					if (!handled) {
						Monitor *pg_mon = pc_gaming_visible_monitor();
						if (pg_mon && htpc_view_is_active(pg_mon, pg_mon->pc_gaming.view_tag, pg_mon->pc_gaming.visible)) {
							PcGamingView *pg = &pg_mon->pc_gaming;
							/* Handle install popup if visible */
							if (pg->install_popup_visible) {
								if (ev.value == -1 && pg->install_popup_selected > 0) {
									pg->install_popup_selected--;
									pc_gaming_install_popup_render(pg_mon);
									handled = 1;
								} else if (ev.value == 1 && pg->install_popup_selected < 1) {
									pg->install_popup_selected++;
									pc_gaming_install_popup_render(pg_mon);
									handled = 1;
								}
							} else {
								if (ev.value == -1 && pg->selected_idx > 0) {
									pg->selected_idx--;
									pc_gaming_render(pg_mon);
									handled = 1;
								} else if (ev.value == 1 && pg->selected_idx < pg->game_count - 1) {
									pg->selected_idx++;
									pc_gaming_render(pg_mon);
									handled = 1;
								}
							}
						}
					}

					/* Check Media views on any monitor */
					if (!handled) {
						Monitor *media_mon = media_view_visible_monitor();
						if (media_mon) {
							if (htpc_view_is_active(media_mon, media_mon->movies_view.view_tag, media_mon->movies_view.visible)) {
								media_view_handle_button(media_mon, MEDIA_VIEW_MOVIES, button, 1);
							} else if (htpc_view_is_active(media_mon, media_mon->tvshows_view.view_tag, media_mon->tvshows_view.visible)) {
								media_view_handle_button(media_mon, MEDIA_VIEW_TVSHOWS, button, 1);
							}
						}
					}
				} else {
					/* D-pad released - stop OSK repeat if active */
					if (osk_dpad_held_button == BTN_DPAD_LEFT || osk_dpad_held_button == BTN_DPAD_RIGHT)
						osk_dpad_repeat_stop();
					/* Stop video player hold-to-seek on D-pad release */
					if (active_videoplayer && active_videoplayer->control_bar.seek_hold_active) {
						videoplayer_seek_hold_stop(active_videoplayer);
						render_playback_osd();
					}
				}
				break;
			case ABS_HAT0Y:
				/* D-pad up/down for menu/grid navigation */
				if (ev.value != 0) {
					int handled = 0;
					int button = ev.value < 0 ? BTN_DPAD_UP : BTN_DPAD_DOWN;

					/* Check OSK first - highest priority */
					Monitor *osk_mon_y = osk_visible_monitor();
					if (osk_mon_y) {
						handled = osk_handle_button(osk_mon_y, button, 1);
						/* Start repeat timer for hold-to-slide */
						osk_dpad_repeat_start(osk_mon_y, button);
					}

					/* Check gamepad menu on selmon first (uses m) */
					if (m && m->gamepad_menu.visible) {
						GamepadMenu *gm = &m->gamepad_menu;
						if (ev.value == -1 && gm->selected > 0) {
							gm->selected--;
							gamepad_menu_render(m);
							handled = 1;
						} else if (ev.value == 1 && gm->selected < gm->item_count - 1) {
							gm->selected++;
							gamepad_menu_render(m);
							handled = 1;
						}
					}

					/* Check Retro gaming on any monitor (must be active on current tag) */
					if (!handled) {
						Monitor *rg_mon = retro_gaming_visible_monitor();
						if (rg_mon && htpc_view_is_active(rg_mon, rg_mon->retro_gaming.view_tag, rg_mon->retro_gaming.visible)) {
							handled = retro_gaming_handle_button(rg_mon, button, 1);
						}
					}

					/* Check PC gaming on any monitor */
					if (!handled) {
						Monitor *pg_mon = pc_gaming_visible_monitor();
						if (pg_mon && htpc_view_is_active(pg_mon, pg_mon->pc_gaming.view_tag, pg_mon->pc_gaming.visible)) {
							PcGamingView *pg = &pg_mon->pc_gaming;
							/* Skip if install popup is visible */
							if (!pg->install_popup_visible) {
								if (ev.value == -1 && pg->selected_idx >= pg->cols) {
									pg->selected_idx -= pg->cols;
									pc_gaming_render(pg_mon);
									handled = 1;
								} else if (ev.value == 1 && pg->selected_idx + pg->cols < pg->game_count) {
									pg->selected_idx += pg->cols;
									pc_gaming_render(pg_mon);
									handled = 1;
								}
							}
						}
					}

					/* Check Media views on any monitor */
					if (!handled) {
						Monitor *media_mon = media_view_visible_monitor();
						if (media_mon) {
							if (htpc_view_is_active(media_mon, media_mon->movies_view.view_tag, media_mon->movies_view.visible)) {
								media_view_handle_button(media_mon, MEDIA_VIEW_MOVIES, button, 1);
							} else if (htpc_view_is_active(media_mon, media_mon->tvshows_view.view_tag, media_mon->tvshows_view.visible)) {
								media_view_handle_button(media_mon, MEDIA_VIEW_TVSHOWS, button, 1);
							}
						}
					}
				} else {
					/* D-pad released - stop OSK repeat if active */
					if (osk_dpad_held_button == BTN_DPAD_UP || osk_dpad_held_button == BTN_DPAD_DOWN)
						osk_dpad_repeat_stop();
				}
				break;
			}
		}
	}

	/* Check for read errors indicating device disconnection */
	if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
		wlr_log(WLR_INFO, "Gamepad read error (errno=%d): %s (%s)", errno, gp->name, gp->path);
		if (gp->event_source) {
			wl_event_source_remove(gp->event_source);
			gp->event_source = NULL;
		}
		if (gp->fd >= 0) {
			close(gp->fd);
			gp->fd = -1;
		}
		wl_list_remove(&gp->link);
		free(gp);
		return 0;
	}

	return 0;
}

void
gamepad_device_add(const char *path)
{
	GamepadDevice *gp, *existing;
	int fd;
	char name[128] = "Unknown";

	if (!path)
		return;

	/* Check if already added */
	wl_list_for_each(existing, &gamepads, link) {
		if (strcmp(existing->path, path) == 0)
			return;
	}

	fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		return;

	/* Check if it's a gamepad */
	if (!gamepad_is_gamepad_device(fd)) {
		close(fd);
		return;
	}

	/* Get device name */
	ioctl(fd, EVIOCGNAME(sizeof(name)), name);

	gp = ecalloc(1, sizeof(*gp));
	gp->fd = fd;
	snprintf(gp->path, sizeof(gp->path), "%s", path);
	snprintf(gp->name, sizeof(gp->name), "%s", name);

	/* Read axis calibration from device */
	struct input_absinfo absinfo;
	/* Left stick X */
	if (ioctl(fd, EVIOCGABS(ABS_X), &absinfo) == 0) {
		gp->cal_lx.min = absinfo.minimum;
		gp->cal_lx.max = absinfo.maximum;
		gp->cal_lx.center = (absinfo.minimum + absinfo.maximum) / 2;
		gp->cal_lx.flat = absinfo.flat;
		gp->left_x = gp->cal_lx.center;
	} else {
		gp->cal_lx.min = -32768; gp->cal_lx.max = 32767; gp->cal_lx.center = 0; gp->cal_lx.flat = 0;
		gp->left_x = 0;
	}
	/* Left stick Y */
	if (ioctl(fd, EVIOCGABS(ABS_Y), &absinfo) == 0) {
		gp->cal_ly.min = absinfo.minimum;
		gp->cal_ly.max = absinfo.maximum;
		gp->cal_ly.center = (absinfo.minimum + absinfo.maximum) / 2;
		gp->cal_ly.flat = absinfo.flat;
		gp->left_y = gp->cal_ly.center;
	} else {
		gp->cal_ly.min = -32768; gp->cal_ly.max = 32767; gp->cal_ly.center = 0; gp->cal_ly.flat = 0;
		gp->left_y = 0;
	}
	/* Right stick X */
	if (ioctl(fd, EVIOCGABS(ABS_RX), &absinfo) == 0) {
		gp->cal_rx.min = absinfo.minimum;
		gp->cal_rx.max = absinfo.maximum;
		gp->cal_rx.center = (absinfo.minimum + absinfo.maximum) / 2;
		gp->cal_rx.flat = absinfo.flat;
		gp->right_x = gp->cal_rx.center;
	} else {
		gp->cal_rx.min = -32768; gp->cal_rx.max = 32767; gp->cal_rx.center = 0; gp->cal_rx.flat = 0;
		gp->right_x = 0;
	}
	/* Right stick Y */
	if (ioctl(fd, EVIOCGABS(ABS_RY), &absinfo) == 0) {
		gp->cal_ry.min = absinfo.minimum;
		gp->cal_ry.max = absinfo.maximum;
		gp->cal_ry.center = (absinfo.minimum + absinfo.maximum) / 2;
		gp->cal_ry.flat = absinfo.flat;
		gp->right_y = gp->cal_ry.center;
	} else {
		gp->cal_ry.min = -32768; gp->cal_ry.max = 32767; gp->cal_ry.center = 0; gp->cal_ry.flat = 0;
		gp->right_y = 0;
	}

	wlr_log(WLR_INFO, "Gamepad calibration: LX[%d-%d c=%d], LY[%d-%d c=%d]",
		gp->cal_lx.min, gp->cal_lx.max, gp->cal_lx.center,
		gp->cal_ly.min, gp->cal_ly.max, gp->cal_ly.center);

	/* Initialize activity tracking */
	gp->last_activity_ms = monotonic_msec();
	gp->suspended = 0;
	gp->grabbed = 0;
	/* Record connect time - ignore button presses within 500ms to filter
	 * synthetic events Bluetooth controllers send on reconnect */
	gp->connect_time_ms = monotonic_msec();

	/* Don't grab gamepad - let Steam and other apps receive input normally */

	/* Add to event loop */
	gp->event_source = wl_event_loop_add_fd(event_loop, fd,
		WL_EVENT_READABLE, gamepad_event_cb, gp);

	/* Start timers if this is the first gamepad */
	if (wl_list_empty(&gamepads)) {
		if (gamepad_cursor_timer)
			wl_event_source_timer_update(gamepad_cursor_timer, GAMEPAD_CURSOR_INTERVAL_MS);
		if (gamepad_inactivity_timer)
			wl_event_source_timer_update(gamepad_inactivity_timer, GAMEPAD_INACTIVITY_CHECK_MS);
	}

	wl_list_insert(&gamepads, &gp->link);

	wlr_log(WLR_INFO, "Gamepad connected: %s (%s)", name, path);

	/* Pause video playback on controller connect to prevent A/V desync.
	 * Bluetooth connection causes PipeWire audio rerouting which breaks sync.
	 * User can resume with play button for clean resync. */
	if (active_videoplayer && playback_state == PLAYBACK_PLAYING) {
		videoplayer_pause(active_videoplayer);
		render_playback_osd();
	}

	/* Switch TV/Monitor to this HDMI input via CEC */
	cec_switch_to_active_source();

	/* Update grab state for new gamepad (grab if in HTPC mode on non-Steam tag) */
	gamepad_update_grab_state();
}

void
gamepad_device_remove(const char *path)
{
	GamepadDevice *gp, *tmp;

	if (!path)
		return;

	wl_list_for_each_safe(gp, tmp, &gamepads, link) {
		if (strcmp(gp->path, path) == 0) {
			wlr_log(WLR_INFO, "Gamepad disconnected: %s (%s)", gp->name, gp->path);
			if (gp->event_source)
				wl_event_source_remove(gp->event_source);
			if (gp->fd >= 0)
				close(gp->fd);
			wl_list_remove(&gp->link);
			free(gp);
			return;
		}
	}
}

void
gamepad_grab(GamepadDevice *gp)
{
	if (!gp || gp->fd < 0 || gp->grabbed)
		return;

	if (ioctl(gp->fd, EVIOCGRAB, 1) == 0) {
		gp->grabbed = 1;
		wlr_log(WLR_INFO, "Gamepad grabbed: %s", gp->name);
	}
}

void
gamepad_ungrab(GamepadDevice *gp)
{
	if (!gp || gp->fd < 0 || !gp->grabbed)
		return;

	if (ioctl(gp->fd, EVIOCGRAB, 0) == 0) {
		gp->grabbed = 0;
		wlr_log(WLR_INFO, "Gamepad ungrabbed: %s", gp->name);
	}
}

int
gamepad_should_grab(void)
{
	Client *focused;
	unsigned int steam_tag;

	/* Only grab in HTPC mode */
	if (!htpc_mode_active)
		return 0;

	/* Steam is on tag 4 (index 3) */
	steam_tag = 1 << 3;

	/* If we're on the Steam tag, let Steam have the gamepad */
	if (selmon && (selmon->tagset[selmon->seltags] & steam_tag))
		return 0;

	/* If Steam or a Steam game is focused, let it have the gamepad */
	focused = focustop(selmon);
	if (focused && (is_steam_client(focused) || is_steam_game(focused)))
		return 0;

	/* We're on a non-Steam tag in HTPC mode - grab for our UI */
	return 1;
}

void
gamepad_update_grab_state(void)
{
	GamepadDevice *gp;
	int should_grab;

	should_grab = gamepad_should_grab();

	wl_list_for_each(gp, &gamepads, link) {
		if (gp->suspended)
			continue;
		if (should_grab && !gp->grabbed) {
			gamepad_grab(gp);
		} else if (!should_grab && gp->grabbed) {
			gamepad_ungrab(gp);
		}
	}
}

int
gamepad_pending_timer_cb(void *data)
{
	int i;

	(void)data;

	for (i = 0; i < gamepad_pending_count; i++) {
		gamepad_device_add(gamepad_pending_paths[i]);
	}
	gamepad_pending_count = 0;

	return 0;
}

int
gamepad_inotify_cb(int fd, uint32_t mask, void *data)
{
	char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
	const struct inotify_event *event;
	ssize_t len;
	char *ptr;
	char path[128];
	int schedule_pending = 0;

	(void)mask;
	(void)data;

	while ((len = read(fd, buf, sizeof(buf))) > 0) {
		for (ptr = buf; ptr < buf + len;
		     ptr += sizeof(struct inotify_event) + event->len) {
			event = (const struct inotify_event *)ptr;

			/* Only interested in event* files */
			if (!event->name || strncmp(event->name, "event", 5) != 0)
				continue;

			snprintf(path, sizeof(path), "/dev/input/%s", event->name);

			if (event->mask & IN_CREATE) {
				/* Queue device for delayed add (non-blocking) */
				if (gamepad_pending_count < GAMEPAD_PENDING_MAX) {
					snprintf(gamepad_pending_paths[gamepad_pending_count],
						sizeof(gamepad_pending_paths[0]), "%s", path);
					gamepad_pending_count++;
					schedule_pending = 1;
				}
			} else if (event->mask & IN_DELETE) {
				gamepad_device_remove(path);
			}
		}
	}

	/* Schedule timer to add pending devices after delay */
	if (schedule_pending && gamepad_pending_timer) {
		wl_event_source_timer_update(gamepad_pending_timer, 100); /* 100ms */
	}

	return 0;
}

void
gamepad_scan_devices(void)
{
	DIR *dir;
	struct dirent *entry;
	char path[128];

	dir = opendir("/dev/input");
	if (!dir)
		return;

	while ((entry = readdir(dir)) != NULL) {
		if (strncmp(entry->d_name, "event", 5) != 0)
			continue;

		snprintf(path, sizeof(path), "/dev/input/%s", entry->d_name);
		gamepad_device_add(path);
	}

	closedir(dir);
}

/* Read left stick X/Y nav direction from all gamepads (-1/0/1 per axis) */
static void
gamepad_read_nav_xy(int *out_x, int *out_y)
{
	GamepadDevice *gp;

	*out_x = 0;
	*out_y = 0;
	wl_list_for_each(gp, &gamepads, link) {
		if (gp->suspended)
			continue;

		int lx_offset = gp->left_x - gp->cal_lx.center;
		int ly_offset = gp->left_y - gp->cal_ly.center;
		int lx_range = (gp->cal_lx.max - gp->cal_lx.min) / 2;
		int ly_range = (gp->cal_ly.max - gp->cal_ly.min) / 2;
		if (lx_range == 0) lx_range = 32767;
		if (ly_range == 0) ly_range = 32767;

		/* Threshold at 50% deflection for navigation */
		int nav_threshold_x = lx_range / 2;
		int nav_threshold_y = ly_range / 2;

		if (lx_offset < -nav_threshold_x) *out_x = -1;
		else if (lx_offset > nav_threshold_x) *out_x = 1;
		if (out_y) {
			if (ly_offset < -nav_threshold_y) *out_y = -1;
			else if (ly_offset > nav_threshold_y) *out_y = 1;
		}
	}
}

/* Check nav repeat timing, returns 1 if a step should fire */
static int
gamepad_nav_should_step(int active, uint64_t now)
{
	if (active) {
		uint64_t delay = joystick_nav_repeat_started ?
				JOYSTICK_NAV_REPEAT_RATE : JOYSTICK_NAV_INITIAL_DELAY;
		if (joystick_nav_last_move == 0 || (now - joystick_nav_last_move) >= delay) {
			joystick_nav_last_move = now;
			joystick_nav_repeat_started = 1;
			return 1;
		}
		return 0;
	}
	/* Reset repeat state when joystick returns to center */
	joystick_nav_last_move = 0;
	joystick_nav_repeat_started = 0;
	return 0;
}

/* Send right stick scroll from all gamepads */
static void
gamepad_send_right_stick_scroll(void)
{
	GamepadDevice *gp;

	wl_list_for_each(gp, &gamepads, link) {
		if (gp->suspended)
			continue;

		int ry_offset = gp->right_y - gp->cal_ry.center;
		int ry_range = (gp->cal_ry.max - gp->cal_ry.min) / 2;
		if (ry_range == 0) ry_range = 32767;
		int ry_deadzone = gp->cal_ry.flat > 0 ? gp->cal_ry.flat :
				(ry_range * GAMEPAD_DEADZONE / 32767);

		if (abs(ry_offset) > ry_deadzone) {
			double ny = (double)ry_offset / (double)ry_range;
			if (ny > 1.0) ny = 1.0;
			if (ny < -1.0) ny = -1.0;

			double scroll_amount = ny * 3.0;
			uint32_t time_msec = (uint32_t)(monotonic_msec() & 0xFFFFFFFF);

			wlr_seat_pointer_notify_axis(seat, time_msec,
				WL_POINTER_AXIS_VERTICAL_SCROLL,
				scroll_amount, (int32_t)(scroll_amount * 120),
				WL_POINTER_AXIS_SOURCE_CONTINUOUS,
				WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
		}
	}
}

void
gamepad_update_cursor(void)
{
	GamepadDevice *gp;
	double total_dx = 0, total_dy = 0;
	int any_input = 0;
	uint64_t now = monotonic_msec();

	if (wl_list_empty(&gamepads))
		return;

	/* Handle video player joystick navigation (left stick = sound/subtitle menu) */
	if (active_videoplayer && playback_state == PLAYBACK_PLAYING) {
		int nav_x = 0, nav_y = 0;
		gamepad_read_nav_xy(&nav_x, &nav_y);

		if (gamepad_nav_should_step(nav_x != 0 || nav_y != 0, now)) {
			int btn = 0;
			if (nav_x == -1) btn = BTN_DPAD_LEFT;
			else if (nav_x == 1) btn = BTN_DPAD_RIGHT;
			else if (nav_y == -1) btn = BTN_DPAD_UP;
			else if (nav_y == 1) btn = BTN_DPAD_DOWN;
			if (btn)
				handle_playback_osd_input(btn);
		}
		return;
	}

	/* Handle Retro gaming view joystick navigation */
	if (selmon && htpc_view_is_active(selmon, selmon->retro_gaming.view_tag, selmon->retro_gaming.visible)) {
		int nav_x = 0, nav_y_unused = 0;
		gamepad_read_nav_xy(&nav_x, &nav_y_unused);

		if (gamepad_nav_should_step(nav_x != 0, now))
			retro_gaming_handle_button(selmon, nav_x < 0 ? BTN_DPAD_LEFT : BTN_DPAD_RIGHT, 1);

		gamepad_send_right_stick_scroll();
		return;
	}

	/* Handle PC gaming view joystick navigation */
	if (selmon && htpc_view_is_active(selmon, selmon->pc_gaming.view_tag, selmon->pc_gaming.visible)) {
		PcGamingView *pg = &selmon->pc_gaming;
		Client *popup_client = focustop(selmon);

		/* If a window has focus (e.g. Steam install dialog), allow mouse control */
		if (popup_client && !popup_client->isfullscreen) {
			/* Show cursor when controlling popup window */
			wlr_cursor_set_xcursor(cursor, cursor_mgr, "default");
			/* Fall through to normal mouse control code below */
		} else {
			/* No popup window - use grid navigation */
			int nav_x = 0, nav_y = 0;
			gamepad_read_nav_xy(&nav_x, &nav_y);

			if (gamepad_nav_should_step(nav_x != 0 || nav_y != 0, now)) {
				if (pg->install_popup_visible) {
					/* Navigate install popup */
					if (nav_x == -1 && pg->install_popup_selected > 0) {
						pg->install_popup_selected--;
						pc_gaming_install_popup_render(selmon);
					} else if (nav_x == 1 && pg->install_popup_selected < 1) {
						pg->install_popup_selected++;
						pc_gaming_install_popup_render(selmon);
					}
				} else {
					/* Navigate game grid */
					int new_idx = pg->selected_idx;
					if (nav_x == -1 && new_idx > 0)
						new_idx--;
					else if (nav_x == 1 && new_idx < pg->game_count - 1)
						new_idx++;
					if (nav_y == -1 && new_idx >= pg->cols)
						new_idx -= pg->cols;
					else if (nav_y == 1 && new_idx + pg->cols < pg->game_count)
						new_idx += pg->cols;

					if (new_idx != pg->selected_idx) {
						pg->selected_idx = new_idx;
						pc_gaming_render(selmon);
					}
				}
			}

			gamepad_send_right_stick_scroll();

			/* Don't move mouse cursor while in PC gaming view without popup */
			return;
		}
	}

	/* Handle Media views (Movies/TV-shows) joystick navigation */
	{
		Monitor *media_mon = media_view_visible_monitor();
		if (media_mon) {
			MediaGridView *view = NULL;
			MediaViewType view_type = MEDIA_VIEW_MOVIES;

			if (htpc_view_is_active(media_mon, media_mon->movies_view.view_tag, media_mon->movies_view.visible)) {
				view = &media_mon->movies_view;
				view_type = MEDIA_VIEW_MOVIES;
			} else if (htpc_view_is_active(media_mon, media_mon->tvshows_view.view_tag, media_mon->tvshows_view.visible)) {
				view = &media_mon->tvshows_view;
				view_type = MEDIA_VIEW_TVSHOWS;
			}

			if (view) {
				int nav_x = 0, nav_y = 0;
				gamepad_read_nav_xy(&nav_x, &nav_y);

				if (gamepad_nav_should_step(nav_x != 0 || nav_y != 0, now)) {
					/* Handle detail view navigation for TV shows */
					if (view->in_detail_view && view_type == MEDIA_VIEW_TVSHOWS) {
						int needs_render = 0;

						/* Left/Right switches between seasons and episodes */
						if (nav_x == -1 && view->detail_focus == DETAIL_FOCUS_EPISODES) {
							view->detail_focus = DETAIL_FOCUS_SEASONS;
							needs_render = 1;
						} else if (nav_x == 1 && view->detail_focus == DETAIL_FOCUS_SEASONS && view->episode_count > 0) {
							view->detail_focus = DETAIL_FOCUS_EPISODES;
							needs_render = 1;
						}

						/* Up/Down navigates within the current column */
						if (view->detail_focus == DETAIL_FOCUS_SEASONS) {
							if (nav_y == -1 && view->selected_season_idx > 0) {
								view->selected_season_idx--;
								MediaSeason *s = view->seasons;
								for (int i = 0; i < view->selected_season_idx && s; i++)
									s = s->next;
								if (s) {
									view->selected_season = s->season;
									const char *show = view->detail_item->title[0] ?
									                   view->detail_item->title : view->detail_item->show_name;
									media_view_fetch_episodes(view, show, s->season);
									view->selected_episode_idx = 0;
									view->episode_scroll_offset = 0;
								}
								needs_render = 1;
							} else if (nav_y == 1 && view->selected_season_idx < view->season_count - 1) {
								view->selected_season_idx++;
								MediaSeason *s = view->seasons;
								for (int i = 0; i < view->selected_season_idx && s; i++)
									s = s->next;
								if (s) {
									view->selected_season = s->season;
									const char *show = view->detail_item->title[0] ?
									                   view->detail_item->title : view->detail_item->show_name;
									media_view_fetch_episodes(view, show, s->season);
									view->selected_episode_idx = 0;
									view->episode_scroll_offset = 0;
								}
								needs_render = 1;
							}
						} else if (view->detail_focus == DETAIL_FOCUS_EPISODES) {
							if (nav_y == -1 && view->selected_episode_idx > 0) {
								view->selected_episode_idx--;
								needs_render = 1;
							} else if (nav_y == 1 && view->selected_episode_idx < view->episode_count - 1) {
								view->selected_episode_idx++;
								needs_render = 1;
							}
						}

						if (needs_render)
							media_view_render_detail(media_mon, view_type);
					} else {
						/* Grid view navigation */
						int cols = view->cols > 0 ? view->cols : 5;
						int old_idx = view->selected_idx;

						if (nav_x == -1 && view->selected_idx > 0)
							view->selected_idx--;
						else if (nav_x == 1 && view->selected_idx < view->item_count - 1)
							view->selected_idx++;
						if (nav_y == -1 && view->selected_idx >= cols)
							view->selected_idx -= cols;
						else if (nav_y == 1 && view->selected_idx + cols < view->item_count)
							view->selected_idx += cols;

						if (view->selected_idx != old_idx)
							media_view_render(media_mon, view_type);
					}
				}

				gamepad_send_right_stick_scroll();

				/* Don't move mouse cursor while in media view */
				return;
			}
		}
	}

	/* Check if we should skip cursor movement for fullscreen clients
	 * (let game handle joystick input), but always allow right stick scrolling */
	int skip_cursor_move = 0;
	Client *focused = focustop(selmon);
	if (focused && focused->isfullscreen) {
		/* In HTPC mode, allow cursor control for browsers (kiosk mode) */
		if (htpc_mode_active && is_browser_client(focused)) {
			/* Browser in fullscreen - allow gamepad cursor control and scroll */
		} else if (htpc_mode_active) {
			/* Check for Steam popup that needs mouse control */
			Client *popup = NULL;
			Client *c;
			wl_list_for_each(c, &clients, link) {
				if (c->isfloating && !c->isfullscreen && is_steam_popup(c)) {
					popup = c;
					break;
				}
			}
			if (!popup) {
				/* No popup - skip cursor but still allow scroll for focused client */
				skip_cursor_move = 1;
			}
			/* There's a Steam popup - allow mouse control below */
		} else if (is_browser_client(focused)) {
			/* Browser fullscreen outside HTPC mode - still allow scroll */
		} else {
			/* Fullscreen game - skip cursor but allow scroll to pass through */
			skip_cursor_move = 1;
		}
	}

	/* Also skip cursor movement if physical mouse was used recently */
	if (last_pointer_motion_ms && (now - last_pointer_motion_ms) < 100)
		skip_cursor_move = 1;

	/* Collect scroll input from right sticks */
	double scroll_x = 0, scroll_y = 0;
	int any_scroll = 0;

	wl_list_for_each(gp, &gamepads, link) {
		double dx = 0, dy = 0;
		double magnitude, normalized;

		/* Skip suspended gamepads */
		if (gp->suspended)
			continue;

		/* Normalize axis values relative to calibrated center */
		int lx_offset = gp->left_x - gp->cal_lx.center;
		int ly_offset = gp->left_y - gp->cal_ly.center;

		/* Calculate range for normalization (half the total range) */
		int lx_range = (gp->cal_lx.max - gp->cal_lx.min) / 2;
		int ly_range = (gp->cal_ly.max - gp->cal_ly.min) / 2;
		if (lx_range == 0) lx_range = 32767;
		if (ly_range == 0) ly_range = 32767;

		/* Calculate deadzone threshold based on device flat value or default */
		int lx_deadzone = gp->cal_lx.flat > 0 ? gp->cal_lx.flat : (lx_range * GAMEPAD_DEADZONE / 32767);
		int ly_deadzone = gp->cal_ly.flat > 0 ? gp->cal_ly.flat : (ly_range * GAMEPAD_DEADZONE / 32767);

		/* Apply deadzone to left stick */
		if (abs(lx_offset) > lx_deadzone || abs(ly_offset) > ly_deadzone) {
			/* Normalize to -1.0 to 1.0 range based on calibration */
			double nx = (double)lx_offset / (double)lx_range;
			double ny = (double)ly_offset / (double)ly_range;

			/* Clamp values */
			if (nx > 1.0) nx = 1.0;
			if (nx < -1.0) nx = -1.0;
			if (ny > 1.0) ny = 1.0;
			if (ny < -1.0) ny = -1.0;

			/* Calculate magnitude for acceleration */
			magnitude = sqrt(nx * nx + ny * ny);
			if (magnitude > 1.0)
				magnitude = 1.0;

			/* Apply acceleration curve (higher values accelerate more) */
			normalized = magnitude * magnitude * GAMEPAD_CURSOR_ACCEL;
			if (normalized < 1.0)
				normalized = 1.0;

			/* Calculate delta movement */
			dx = nx * GAMEPAD_CURSOR_SPEED * normalized;
			dy = ny * GAMEPAD_CURSOR_SPEED * normalized;

			any_input = 1;
		}

		/* Right stick - normalize relative to calibrated center */
		int rx_offset = gp->right_x - gp->cal_rx.center;
		int ry_offset = gp->right_y - gp->cal_ry.center;
		int rx_range = (gp->cal_rx.max - gp->cal_rx.min) / 2;
		int ry_range = (gp->cal_ry.max - gp->cal_ry.min) / 2;
		if (rx_range == 0) rx_range = 32767;
		if (ry_range == 0) ry_range = 32767;
		int rx_deadzone = gp->cal_rx.flat > 0 ? gp->cal_rx.flat : (rx_range * GAMEPAD_DEADZONE / 32767);
		int ry_deadzone = gp->cal_ry.flat > 0 ? gp->cal_ry.flat : (ry_range * GAMEPAD_DEADZONE / 32767);

		/* Collect right stick scroll values */
		if (abs(ry_offset) > ry_deadzone) {
			double ny = (double)ry_offset / (double)ry_range;
			if (ny > 1.0) ny = 1.0;
			if (ny < -1.0) ny = -1.0;
			scroll_y += ny * 3.0;
			any_scroll = 1;
		}

		if (abs(rx_offset) > rx_deadzone) {
			double nx = (double)rx_offset / (double)rx_range;
			if (nx > 1.0) nx = 1.0;
			if (nx < -1.0) nx = -1.0;
			scroll_x += nx * 3.0;
			any_scroll = 1;
		}

		total_dx += dx;
		total_dy += dy;
	}

	/* Move cursor if there's any input (skip if physical mouse was used recently) */
	if (!skip_cursor_move && any_input && cursor && (fabs(total_dx) > 0.1 || fabs(total_dy) > 0.1)) {
		/* Move cursor directly (fast) */
		wlr_cursor_move(cursor, NULL, total_dx, total_dy);
		wlr_idle_notifier_v1_notify_activity(idle_notifier, seat);

		/* Update hover feedback for statusbar */
		if (selmon && selmon->showbar) {
			updatetaghover(selmon, cursor->x, cursor->y);
			updatenethover(selmon, cursor->x, cursor->y);
		}
	}

	/* Update pointer focus for cursor movement */
	if (!skip_cursor_move && any_input && cursor) {
		double sx, sy;
		struct wlr_surface *surface = NULL;
		Client *c = NULL;
		xytonode(cursor->x, cursor->y, &surface, &c, NULL, &sx, &sy);
		if (surface) {
			uint32_t time_msec = (uint32_t)(now & 0xFFFFFFFF);
			wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
			wlr_seat_pointer_notify_motion(seat, time_msec, sx, sy);
		}
	}

	/* Send scroll events to focused window (not cursor position) */
	if (any_scroll) {
		Client *scroll_target = focustop(selmon);
		if (scroll_target && client_surface(scroll_target) && client_surface(scroll_target)->mapped) {
			struct wlr_surface *target_surface = client_surface(scroll_target);
			uint32_t time_msec = (uint32_t)(now & 0xFFFFFFFF);

			/* Calculate surface-local coordinates (center of window) */
			double sx = scroll_target->geom.width / 2.0;
			double sy = scroll_target->geom.height / 2.0;

			/* Enter pointer focus on the focused window for scroll events */
			wlr_seat_pointer_notify_enter(seat, target_surface, sx, sy);

			if (fabs(scroll_y) > 0.01) {
				wlr_seat_pointer_notify_axis(seat, time_msec,
					WL_POINTER_AXIS_VERTICAL_SCROLL,
					scroll_y, (int32_t)(scroll_y * 120),
					WL_POINTER_AXIS_SOURCE_CONTINUOUS,
					WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
			}
			if (fabs(scroll_x) > 0.01) {
				wlr_seat_pointer_notify_axis(seat, time_msec,
					WL_POINTER_AXIS_HORIZONTAL_SCROLL,
					scroll_x, (int32_t)(scroll_x * 120),
					WL_POINTER_AXIS_SOURCE_CONTINUOUS,
					WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
			}
			wlr_idle_notifier_v1_notify_activity(idle_notifier, seat);
		}
	}
}

int
gamepad_cursor_timer_cb(void *data)
{
	GamepadDevice *gp;
	Monitor *m;
	int64_t now;

	(void)data;

	/* Update cursor position */
	gamepad_update_cursor();


	/* Reschedule timer if we have gamepads */
	if (!wl_list_empty(&gamepads) && gamepad_cursor_timer) {
		wl_event_source_timer_update(gamepad_cursor_timer, GAMEPAD_CURSOR_INTERVAL_MS);
	}

	return 0;
}

int
gamepad_any_monitor_active(void)
{
	Monitor *m;

	wl_list_for_each(m, &mons, link) {
		if (m->wlr_output && m->wlr_output->enabled && !m->asleep)
			return 1;
	}
	return 0;
}

void
gamepad_turn_off_led_sysfs(GamepadDevice *gp)
{
	DIR *dir;
	struct dirent *entry;
	char event_name[32];
	char led_path[256];
	char sysfs_path[256];
	const char *event_part;
	int found = 0;

	if (!gp || !gp->path[0])
		return;

	/* Extract event number from path (e.g., /dev/input/event5 -> event5) */
	event_part = strrchr(gp->path, '/');
	if (!event_part)
		return;
	event_part++;  /* Skip the '/' */
	snprintf(event_name, sizeof(event_name), "%s", event_part);

	/* Method 1: Check /sys/class/input/eventX/device/device/leds/ (for xpadneo/xone) */
	snprintf(sysfs_path, sizeof(sysfs_path),
		"/sys/class/input/%s/device/device/leds", event_name);
	dir = opendir(sysfs_path);
	if (dir) {
		while ((entry = readdir(dir)) != NULL) {
			if (entry->d_name[0] == '.')
				continue;
			/* Try to write 0 to brightness */
			snprintf(led_path, sizeof(led_path), "%s/%s/brightness",
				sysfs_path, entry->d_name);
			int fd = open(led_path, O_WRONLY);
			if (fd >= 0) {
				write(fd, "0\n", 2);
				close(fd);
				wlr_log(WLR_INFO, "Turned off LED via %s", led_path);
				found = 1;
			}
		}
		closedir(dir);
	}

	/* Method 2: Check /sys/class/leds/ for xpad* entries */
	if (!found) {
		dir = opendir("/sys/class/leds");
		if (dir) {
			while ((entry = readdir(dir)) != NULL) {
				/* Look for xpad LEDs (Xbox 360) or gip* LEDs (xone) */
				if (strncmp(entry->d_name, "xpad", 4) == 0 ||
				    strncmp(entry->d_name, "gip", 3) == 0) {
					snprintf(led_path, sizeof(led_path),
						"/sys/class/leds/%s/brightness", entry->d_name);
					int fd = open(led_path, O_WRONLY);
					if (fd >= 0) {
						/* Pattern 0 = all LEDs off for xpad */
						write(fd, "0\n", 2);
						close(fd);
						wlr_log(WLR_INFO, "Turned off LED via %s", led_path);
						found = 1;
					}
				}
			}
			closedir(dir);
		}
	}

	if (!found) {
		wlr_log(WLR_DEBUG, "No sysfs LED found for gamepad %s", gp->name);
	}
}

void
gamepad_bt_disconnect(GamepadDevice *gp)
{
	int r;
	char *name_copy;

	if (!bt_bus || !gp)
		return;

	/* Copy name since gp may be freed before async callback */
	name_copy = strdup(gp->name);
	if (!name_copy)
		return;

	/* Async call to get managed objects */
	r = sd_bus_call_method_async(bt_bus, NULL,
		"org.bluez",
		"/",
		"org.freedesktop.DBus.ObjectManager",
		"GetManagedObjects",
		bt_get_objects_disconnect_cb,
		name_copy,
		"");

	if (r < 0) {
		wlr_log(WLR_DEBUG, "Failed to start async GetManagedObjects: %s", strerror(-r));
		free(name_copy);
	}
}

void
gamepad_suspend(GamepadDevice *gp)
{
	char name_copy[128];

	if (!gp || gp->suspended)
		return;

	/* Mark as suspended FIRST before any operations that might trigger removal */
	gp->suspended = 1;

	/* Copy name before potential free */
	snprintf(name_copy, sizeof(name_copy), "%s", gp->name);

	/* Turn off LED via sysfs (Xbox controllers) */
	gamepad_turn_off_led_sysfs(gp);

	/* Disconnect Bluetooth controller - this will power it off completely.
	 * WARNING: This may trigger inotify which calls gamepad_device_remove()
	 * and frees gp. Do NOT access gp after this call! */
	gamepad_bt_disconnect(gp);

	/* gp may be freed at this point - use copied name */
	wlr_log(WLR_INFO, "Gamepad powered off due to inactivity: %s", name_copy);
}

void
gamepad_resume(GamepadDevice *gp)
{
	if (!gp || !gp->suspended)
		return;

	gp->suspended = 0;
	gp->last_activity_ms = monotonic_msec();
	wlr_log(WLR_INFO, "Gamepad resumed: %s", gp->name);

	/* Switch TV to active source when gamepad wakes */
	cec_switch_to_active_source();
}

int
gamepad_inactivity_timer_cb(void *data)
{
	GamepadDevice *gp, *tmp;
	int64_t now = monotonic_msec();
	int any_active = 0;
	int monitors_active = gamepad_any_monitor_active();

	(void)data;

	/* Use _safe iteration because gamepad_suspend may trigger device removal */
	wl_list_for_each_safe(gp, tmp, &gamepads, link) {
		if (gp->suspended)
			continue;

		any_active = 1;

		/* Suspend if monitors are off or inactive for too long */
		if (!monitors_active ||
		    (now - gp->last_activity_ms) >= GAMEPAD_INACTIVITY_TIMEOUT_MS) {
			gamepad_suspend(gp);
		}
	}

	/* Reschedule timer if there are active gamepads */
	if (any_active && gamepad_inactivity_timer) {
		wl_event_source_timer_update(gamepad_inactivity_timer, GAMEPAD_INACTIVITY_CHECK_MS);
	}

	return 0;
}

void
gamepad_setup(void)
{
	wl_list_init(&gamepads);

	/* Set up inotify to watch /dev/input for new devices */
	gamepad_inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (gamepad_inotify_fd < 0) {
		wlr_log(WLR_ERROR, "Failed to create gamepad inotify: %s", strerror(errno));
		return;
	}

	gamepad_inotify_wd = inotify_add_watch(gamepad_inotify_fd, "/dev/input",
		IN_CREATE | IN_DELETE);
	if (gamepad_inotify_wd < 0) {
		wlr_log(WLR_ERROR, "Failed to watch /dev/input: %s", strerror(errno));
		close(gamepad_inotify_fd);
		gamepad_inotify_fd = -1;
		return;
	}

	gamepad_inotify_event = wl_event_loop_add_fd(event_loop, gamepad_inotify_fd,
		WL_EVENT_READABLE, gamepad_inotify_cb, NULL);

	/* Create cursor movement timer */
	gamepad_cursor_timer = wl_event_loop_add_timer(event_loop, gamepad_cursor_timer_cb, NULL);

	/* Create pending device timer for delayed add */
	gamepad_pending_timer = wl_event_loop_add_timer(event_loop, gamepad_pending_timer_cb, NULL);

	/* Create inactivity timer for auto-suspend */
	gamepad_inactivity_timer = wl_event_loop_add_timer(event_loop, gamepad_inactivity_timer_cb, NULL);

	/* Scan existing devices */
	gamepad_scan_devices();

	/* Start timers if we found any gamepads */
	if (!wl_list_empty(&gamepads)) {
		if (gamepad_cursor_timer)
			wl_event_source_timer_update(gamepad_cursor_timer, GAMEPAD_CURSOR_INTERVAL_MS);
		if (gamepad_inactivity_timer)
			wl_event_source_timer_update(gamepad_inactivity_timer, GAMEPAD_INACTIVITY_CHECK_MS);
	}

	wlr_log(WLR_INFO, "Gamepad support initialized");
}

void
gamepad_cleanup(void)
{
	GamepadDevice *gp, *tmp;

	/* Remove all gamepads */
	wl_list_for_each_safe(gp, tmp, &gamepads, link) {
		if (gp->event_source)
			wl_event_source_remove(gp->event_source);
		if (gp->fd >= 0)
			close(gp->fd);
		wl_list_remove(&gp->link);
		free(gp);
	}

	/* Clean up cursor timer */
	if (gamepad_cursor_timer) {
		wl_event_source_remove(gamepad_cursor_timer);
		gamepad_cursor_timer = NULL;
	}

	/* Clean up pending device timer */
	if (gamepad_pending_timer) {
		wl_event_source_remove(gamepad_pending_timer);
		gamepad_pending_timer = NULL;
	}
	gamepad_pending_count = 0;

	/* Clean up inactivity timer */
	if (gamepad_inactivity_timer) {
		wl_event_source_remove(gamepad_inactivity_timer);
		gamepad_inactivity_timer = NULL;
	}

	/* Clean up inotify */
	if (gamepad_inotify_event) {
		wl_event_source_remove(gamepad_inotify_event);
		gamepad_inotify_event = NULL;
	}
	if (gamepad_inotify_fd >= 0) {
		if (gamepad_inotify_wd >= 0)
			inotify_rm_watch(gamepad_inotify_fd, gamepad_inotify_wd);
		close(gamepad_inotify_fd);
		gamepad_inotify_fd = -1;
		gamepad_inotify_wd = -1;
	}
}

