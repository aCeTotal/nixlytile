#include "nixlytile.h"
#include "client.h"

static void (*last_keybinding_func)(const Arg *);

void
applyxkbdefaultsfromsystem(struct xkb_rule_names *names)
{
	static int loaded;
	static char layout[128];
	static char model[128];
	static char variant[128];
	static char options[256];
	FILE *fp;
	char line[256];
	char *newline;

	/* If config already provided everything, avoid extra work */
	if (names->model && names->layout && names->variant && names->options)
		return;

	if (!loaded) {
		int need_localectl;
		fp = fopen("/etc/X11/xorg.conf.d/00-keyboard.conf", "r");
		if (fp) {
			while (fgets(line, sizeof(line), fp)) {
				newline = strpbrk(line, "\r\n");
				if (newline)
					*newline = '\0';
				if (!layout[0] && sscanf(line, " Option \"XkbLayout\" \"%127[^\"]\"", layout) == 1)
					continue;
				if (!model[0] && sscanf(line, " Option \"XkbModel\" \"%127[^\"]\"", model) == 1)
					continue;
				if (!variant[0] && sscanf(line, " Option \"XkbVariant\" \"%127[^\"]\"", variant) == 1)
					continue;
				if (!options[0] && sscanf(line, " Option \"XkbOptions\" \"%255[^\"]\"", options) == 1)
					continue;
			}
			fclose(fp);
		}
		need_localectl = !layout[0] || !model[0] || !variant[0] || !options[0];
		/* If the X11 config isn't present (Wayland-only setups) or lacks data, try localectl */
		if (need_localectl) {
			fp = popen("localectl status", "r");
			if (fp) {
				while (fgets(line, sizeof(line), fp)) {
					newline = strpbrk(line, "\r\n");
					if (newline)
						*newline = '\0';
					if (!layout[0] && sscanf(line, " X11 Layout: %127[^\n]", layout) == 1)
						continue;
					if (!model[0] && sscanf(line, " X11 Model: %127[^\n]", model) == 1)
						continue;
					if (!variant[0] && sscanf(line, " X11 Variant: %127[^\n]", variant) == 1)
						continue;
					if (!options[0] && sscanf(line, " X11 Options: %255[^\n]", options) == 1)
						continue;
				}
				pclose(fp);
			}
		}
		loaded = 1;
	}

	if (!names->layout && layout[0])
		names->layout = layout;
	if (!names->model && model[0])
		names->model = model;
	if (!names->variant && variant[0])
		names->variant = variant;
	if (!names->options && options[0])
		names->options = options;
}

struct xkb_rule_names
getxkbrules(void)
{
	struct xkb_rule_names names = xkb_rules;
	const char *env;

	if (!names.rules && (env = getenv("XKB_DEFAULT_RULES")))
		names.rules = env;
	if (!names.model && (env = getenv("XKB_DEFAULT_MODEL")))
		names.model = env;
	if (!names.layout && (env = getenv("XKB_DEFAULT_LAYOUT")))
		names.layout = env;
	if (!names.variant && (env = getenv("XKB_DEFAULT_VARIANT")))
		names.variant = env;
	if (!names.options && (env = getenv("XKB_DEFAULT_OPTIONS")))
		names.options = env;

	/* NixOS/localectl store XKB defaults system-wide; use them as a fallback so
	 * Wayland sessions pick up the same keyboard layout. */
	applyxkbdefaultsfromsystem(&names);

	return names;
}

int
scrollsteps(const struct wlr_pointer_axis_event *event)
{
	int steps;

	if (!event)
		return 0;

	if (event->delta_discrete > 0)
		steps = 1;
	else if (event->delta_discrete < 0)
		steps = -1;
	else if (event->delta > 0.0)
		steps = 1;
	else if (event->delta < 0.0)
		steps = -1;
	else
		steps = 0;

	if (event->relative_direction == WL_POINTER_AXIS_RELATIVE_DIRECTION_INVERTED)
		steps = -steps;

	return steps;
}

int
adjust_backlight_by_steps(int steps)
{
	double percent;
	double delta;

	if (steps == 0)
		return 0;

	/* Re-probe in case a dock/undock changed backlight availability */
	backlight_available = findbacklightdevice(backlight_brightness_path,
			sizeof(backlight_brightness_path),
			backlight_max_path, sizeof(backlight_max_path));

	/* No backlight device = not a laptop, nothing to adjust */
	if (!backlight_available)
		return 0;

	percent = backlight_percent();
	if (percent < 0.0)
		percent = light_last_percent;
	if (percent < 0.0)
		percent = light_cached_percent >= 0.0 ? light_cached_percent : 50.0; /* fallback default */

	delta = (double)steps * light_step;

	/* First try relative adjustment via helper and return on success */
	if (set_backlight_relative(delta) == 0) {
		double newp = backlight_percent();
		if (newp >= 0.0)
			percent = newp;
		else
			percent = percent + delta;
		if (percent < 0.0)
			percent = 0.0;
		if (percent > 100.0)
			percent = 100.0;
		light_last_percent = percent;
		light_cached_percent = percent;
		refreshstatuslight();
		return 1;
	}

	/* Fall back to absolute write */
	{
		double target = percent + delta;
		if (target < 0.0)
			target = 0.0;
		if (target > 100.0)
			target = 100.0;

		if (set_backlight_percent(target) != 0) {
			light_last_percent = target;
			light_cached_percent = target;
			refreshstatuslight();
			return 1;
		}
		light_last_percent = target;
		light_cached_percent = target;
	}

	refreshstatuslight();
	return 1;
}

int
adjust_volume_by_steps(int steps)
{
	double vol;
	int is_headset;
	uint64_t now;

	if (steps == 0)
		return 0;

	is_headset = pipewire_sink_is_headset();
	volume_invalidate_cache(is_headset); /* force fresh read if needed */

	vol = speaker_active >= 0.0 ? speaker_active : volume_last_for_type(is_headset);
	if (vol < 0.0)
		vol = pipewire_volume_percent(&is_headset);
	if (vol < 0.0)
		return 0;

	if (volume_muted == 1) {
		set_pipewire_mute(0);
	}

	vol += (double)steps * volume_step;
	if (vol < 0.0)
		vol = 0.0;
	if (vol > volume_max_percent)
		vol = volume_max_percent;

	if (set_pipewire_volume(vol) != 0)
		return 0;

	now = monotonic_msec();
	volume_cache_store(is_headset, vol, 0, now);
	speaker_active = vol;
	if (is_headset) {
		volume_cached_headset_muted = 0;
	} else {
		volume_cached_speaker_muted = 0;
	}
	refreshstatusvolume();
	return 1;
}

int
adjust_mic_by_steps(int steps)
{
	double vol;
	double target;

	if (steps == 0)
		return 0;

	mic_last_read_ms = 0;
	vol = microphone_active >= 0.0 ? microphone_active : mic_last_percent;
	if (vol < 0.0)
		vol = pipewire_mic_volume_percent();
	if (vol < 0.0)
		return 0;

	if (mic_muted == 1) {
		set_pipewire_mic_mute(0);
	}

	target = vol + (double)steps * mic_step;
	if (target < 0.0)
		target = 0.0;
	if (target > mic_max_percent)
		target = mic_max_percent;

	if (set_pipewire_mic_volume(target) != 0)
		return 0;

	mic_last_percent = target;
	mic_cached = target;
	mic_cached_muted = 0;
	mic_muted = 0;
	mic_last_read_ms = monotonic_msec();
	microphone_active = target;
	refreshstatusmic();
	return 1;
}

int
handlestatusscroll(struct wlr_pointer_axis_event *event)
{
	Monitor *m;
	int lx, ly, steps;

	if (!event || locked)
		return 0;

	if (event->orientation != WL_POINTER_AXIS_VERTICAL_SCROLL)
		return 0;

	m = xytomon(cursor->x, cursor->y);
	if (!m)
		return 0;
	selmon = m;

	if (!m->showbar || !m->statusbar.area.width || !m->statusbar.area.height)
		return 0;

	lx = (int)floor(cursor->x) - m->statusbar.area.x;
	ly = (int)floor(cursor->y) - m->statusbar.area.y;
	if (lx < 0 || ly < 0 || lx >= m->statusbar.area.width || ly >= m->statusbar.area.height)
		return 0;

	steps = -scrollsteps(event);
	if (steps == 0)
		return 0;

	if (m->statusbar.light.width > 0 &&
			lx >= m->statusbar.light.x &&
			lx < m->statusbar.light.x + m->statusbar.light.width) {
		return adjust_backlight_by_steps(steps);
	}

	if (m->statusbar.mic.width > 0 &&
			lx >= m->statusbar.mic.x &&
			lx < m->statusbar.mic.x + m->statusbar.mic.width) {
		return adjust_mic_by_steps(steps);
	}

	if (m->statusbar.volume.width > 0 &&
			lx >= m->statusbar.volume.x &&
			lx < m->statusbar.volume.x + m->statusbar.volume.width) {
		return adjust_volume_by_steps(steps);
	}

	return 0;
}

void
axisnotify(struct wl_listener *listener, void *data)
{
	/* This event is forwarded by the cursor when a pointer emits an axis event,
	 * for example when you move the scroll wheel. */
	struct wlr_pointer_axis_event *event = data;
	wlr_idle_notifier_v1_notify_activity(idle_notifier, seat);
	if (!handlestatusscroll(event)) {
		/* Notify the client with pointer focus of the axis event. */
		wlr_seat_pointer_notify_axis(seat,
				event->time_msec, event->orientation, event->delta,
				event->delta_discrete, event->source, event->relative_direction);
	}
}

/* Returns 1 if click was handled by a statusbar module */
static int
handle_statusbar_clicks(Monitor *m, int lx, int ly, uint32_t button)
{
	StatusModule *tags = &m->statusbar.tags;
	StatusModule *mic = &m->statusbar.mic;
	StatusModule *vol = &m->statusbar.volume;
	StatusModule *cpu = &m->statusbar.cpu;
	StatusModule *fan = &m->statusbar.fan;
	StatusModule *sys = &m->statusbar.sysicons;
	StatusModule *bt = &m->statusbar.bluetooth;
	StatusModule *stm = &m->statusbar.steam;
	StatusModule *dsc = &m->statusbar.discord;

	if (m->statusbar.cpu_popup.visible) {
		if (cpu_popup_handle_click(m, lx, ly, button))
			return 1;
	}

	if (m->statusbar.ram_popup.visible) {
		if (ram_popup_handle_click(m, lx, ly, button))
			return 1;
	}

	if (m->statusbar.fan_popup.visible) {
		if (fan_popup_handle_click(m, lx, ly, button))
			return 1;
	}

	if (lx < 0 || ly < 0 ||
			lx >= m->statusbar.area.width ||
			ly >= m->statusbar.area.height)
		return 0;

	if (button == BTN_RIGHT &&
			sys->width > 0 &&
			lx >= sys->x && lx < sys->x + sys->width) {
		net_menu_hide_all();
		net_menu_open(m);
		return 1;
	}

	if (button == BTN_LEFT &&
			sys->width > 0 &&
			lx >= sys->x && lx < sys->x + sys->width) {
		Arg arg = { .v = netcmd };
		spawn(&arg);
		return 1;
	}

	if (cpu->width > 0 && lx >= cpu->x && lx < cpu->x + cpu->width) {
		Arg arg = { .v = btopcmd };
		spawn(&arg);
		return 1;
	}

	/* Fan module click - toggle fan popup */
	if (button == BTN_LEFT &&
			fan->width > 0 &&
			lx >= fan->x && lx < fan->x + fan->width) {
		FanPopup *fp = &m->statusbar.fan_popup;
		if (fp->visible) {
			fp->visible = 0;
			fp->dragging = 0;
			if (fp->tree)
				wlr_scene_node_set_enabled(&fp->tree->node, 0);
		} else {
			if (fp->device_count == 0)
				fan_scan_hwmon(fp);
			fp->visible = 1;
			if (fp->tree) {
				wlr_scene_node_set_enabled(&fp->tree->node, 1);
				renderfanpopup(m);
				positionstatusmodules(m);
			}
		}
		return 1;
	}

	if (mic->width > 0 && lx >= mic->x && lx < mic->x + mic->width) {
		if (button == BTN_LEFT) {
			toggle_pipewire_mic_mute();
		} else if (button == BTN_RIGHT) {
			Arg arg = { .v = pavucontrolcmd };
			spawn(&arg);
		}
		return 1;
	}

	if (vol->width > 0 && lx >= vol->x && lx < vol->x + vol->width) {
		if (button == BTN_LEFT) {
			toggle_pipewire_mute();
		} else if (button == BTN_RIGHT) {
			Arg arg = { .v = pavucontrolcmd };
			spawn(&arg);
		}
		return 1;
	}

	/* Bluetooth module click - open bluetooth manager */
	if (button == BTN_LEFT &&
			bt->width > 0 &&
			lx >= bt->x && lx < bt->x + bt->width) {
		pid_t pid = fork();
		if (pid == 0) {
			setsid();
			execlp("blueman-manager", "blueman-manager", NULL);
			_exit(1);
		}
		return 1;
	}

	/* Steam module click - focus or launch Steam */
	if (button == BTN_LEFT &&
			stm->width > 0 &&
			lx >= stm->x && lx < stm->x + stm->width) {
		focus_or_launch_app("steam", "steam");
		return 1;
	}

	/* Discord module click - focus or launch Discord */
	if (button == BTN_LEFT &&
			dsc->width > 0 &&
			lx >= dsc->x && lx < dsc->x + dsc->width) {
		focus_or_launch_app("discord", "discord");
		return 1;
	}

	if (button == BTN_LEFT && lx < tags->width) {
		for (int i = 0; i < tags->box_count; i++) {
			int bx = tags->box_x[i];
			int bw = tags->box_w[i];
			if (lx >= bx && lx < bx + bw) {
				Arg arg = { .ui = 1u << tags->box_tag[i] };
				view(&arg);
				return 1;
			}
		}
	}
	return 0;
}

void
handle_pointer_button_internal(uint32_t button, uint32_t state, uint32_t time_msec)
{
	struct wlr_keyboard *keyboard;
	uint32_t mods;
	Client *c;
	const Button *b;

	wlr_idle_notifier_v1_notify_activity(idle_notifier, seat);

	if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
		cursor_mode = CurPressed;
		selmon = xytomon(cursor->x, cursor->y);
		if (locked)
			goto notify_client;

		/* Modal overlay eats clicks; outside closes it */
		{
			Monitor *modal_mon = modal_visible_monitor();
			if (modal_mon) {
				ModalOverlay *mo = &modal_mon->modal;
				int inside = cursor->x >= mo->x && cursor->x < mo->x + mo->width &&
						cursor->y >= mo->y && cursor->y < mo->y + mo->height;
				if (!inside)
					modal_hide(modal_mon);
				return;
			}
		}

		if (selmon && selmon->statusbar.tray_menu.visible) {
			int lx = (int)lround(cursor->x - selmon->statusbar.area.x);
			int ly = (int)lround(cursor->y - selmon->statusbar.area.y);
			TrayMenu *menu = &selmon->statusbar.tray_menu;
			int relx = lx - menu->x;
			int rely = ly - menu->y;
			if (relx >= 0 && rely >= 0 &&
					relx < menu->width && rely < menu->height) {
				TrayMenuEntry *entry = tray_menu_entry_at(selmon, relx, rely);
				if (entry)
					tray_menu_send_event(menu, entry, time_msec);
				tray_menu_hide_all();
				return;
			} else {
				tray_menu_hide_all();
			}
		}

		if (selmon && selmon->wifi_popup.visible) {
			int lx = (int)lround(cursor->x);
			int ly = (int)lround(cursor->y);
			if (wifi_popup_handle_click(selmon, lx, ly, button))
				return;
		}

		if (selmon && selmon->statusbar.net_menu.visible) {
			int lx = (int)lround(cursor->x - selmon->statusbar.area.x);
			int ly = (int)lround(cursor->y - selmon->statusbar.area.y);
			if (net_menu_handle_click(selmon, lx, ly, button))
				return;
		}

		if (selmon && selmon->showbar) {
			int lx = (int)lround(cursor->x - selmon->statusbar.area.x);
			int ly = (int)lround(cursor->y - selmon->statusbar.area.y);
			if (handle_statusbar_clicks(selmon, lx, ly, button))
				return;
		}

		/* Change focus if the button was _pressed_ over a client */
		xytonode(cursor->x, cursor->y, NULL, &c, NULL, NULL, NULL);
		if (c && (!client_is_unmanaged(c) || client_wants_focus(c)))
			focusclient(c, 1);

		keyboard = wlr_seat_get_keyboard(seat);
		mods = keyboard ? wlr_keyboard_get_modifiers(keyboard) : 0;
		for (b = buttons; b < buttons + nbuttons; b++) {
			if (CLEANMASK(mods) == CLEANMASK(b->mod) &&
					button == b->button && b->func) {
				b->func(&b->arg);
				return;
			}
		}
	} else if (state == WL_POINTER_BUTTON_STATE_RELEASED) {
		/* Clear fan slider drag on button release */
		if (selmon && selmon->statusbar.fan_popup.dragging) {
			selmon->statusbar.fan_popup.dragging = 0;
			renderfanpopup(selmon);
		}
		/* Reset cursor mode on release */
		if (cursor_mode == CurPressed)
			cursor_mode = CurNormal;
	}

notify_client:
	/* Notify the client with pointer focus */
	wlr_seat_pointer_notify_button(seat, time_msec, button, state);
}

void
buttonpress(struct wl_listener *listener, void *data)
{
	struct wlr_pointer_button_event *event = data;
	struct wlr_keyboard *keyboard;
	uint32_t mods;
	Client *c, *target = NULL;
	const Button *b;

	wlr_idle_notifier_v1_notify_activity(idle_notifier, seat);

	switch (event->state) {
	case WL_POINTER_BUTTON_STATE_PRESSED:
		cursor_mode = CurPressed;
		selmon = xytomon(cursor->x, cursor->y);
		if (locked)
			break;

		/* Screenshot selection intercepts all clicks */
		if (screenshot_mode >= SCREENSHOT_SELECTING) {
			screenshot_handle_button(event->button, event->state, event->time_msec);
			return;
		}

		/* Monitor setup popup intercepts all clicks */
		{
			Monitor *ms_mon = monitor_setup_visible_monitor();
			if (ms_mon) {
				int cx = (int)lround(cursor->x);
				int cy = (int)lround(cursor->y);
				monitor_setup_handle_button(ms_mon, cx, cy,
					event->button, event->state);
				return;
			}
		}

		/* Modal overlay eats clicks; outside closes it */
		{
			Monitor *modal_mon = modal_visible_monitor();
			if (modal_mon) {
				ModalOverlay *mo = &modal_mon->modal;
				int inside = cursor->x >= mo->x && cursor->x < mo->x + mo->width &&
						cursor->y >= mo->y && cursor->y < mo->y + mo->height;
				if (!inside)
					modal_hide(modal_mon);
				return;
			}
		}

		/* Handle gamepad/HTPC menu clicks */
		if (selmon && selmon->gamepad_menu.visible) {
			int cx = (int)lround(cursor->x);
			int cy = (int)lround(cursor->y);
			if (gamepad_menu_handle_click(selmon, cx, cy, event->button))
				return;
		}

		/*
		 * Skip statusbar and popup click handling when a fullscreen
		 * client covers the monitor.  The fullscreen surface is on
		 * LyrFS which is above LyrTop (statusbar), so the bar is
		 * invisible — clicks must go to the fullscreen client, not
		 * pass through to the hidden statusbar.
		 */
		{
			Client *fs = selmon ? focustop(selmon) : NULL;
			if (fs && fs->isfullscreen)
				goto skip_statusbar;
		}

		if (selmon && selmon->statusbar.tray_menu.visible) {
			int lx = (int)lround(cursor->x - selmon->statusbar.area.x);
			int ly = (int)lround(cursor->y - selmon->statusbar.area.y);
			TrayMenu *menu = &selmon->statusbar.tray_menu;
			int relx = lx - menu->x;
			int rely = ly - menu->y;
			if (relx >= 0 && rely >= 0 &&
					relx < menu->width && rely < menu->height) {
				TrayMenuEntry *entry = tray_menu_entry_at(selmon, relx, rely);
				if (entry)
					tray_menu_send_event(menu, entry, event->time_msec);
				tray_menu_hide_all();
				return;
			} else {
				tray_menu_hide_all();
			}
		}

		if (selmon && selmon->wifi_popup.visible) {
			int lx = (int)lround(cursor->x);
			int ly = (int)lround(cursor->y);
			if (wifi_popup_handle_click(selmon, lx, ly, event->button))
				return;
		}

		if (selmon && selmon->statusbar.net_menu.visible) {
			int lx = (int)lround(cursor->x - selmon->statusbar.area.x);
			int ly = (int)lround(cursor->y - selmon->statusbar.area.y);
			if (net_menu_handle_click(selmon, lx, ly, event->button))
				return;
		}

		if (selmon && selmon->showbar) {
			int lx = (int)lround(cursor->x - selmon->statusbar.area.x);
			int ly = (int)lround(cursor->y - selmon->statusbar.area.y);
			if (handle_statusbar_clicks(selmon, lx, ly, event->button))
				return;
		}

skip_statusbar:

		/* Change focus if the button was _pressed_ over a client */
		xytonode(cursor->x, cursor->y, NULL, &c, NULL, NULL, NULL);
		if (c && (!client_is_unmanaged(c) || client_wants_focus(c)))
			focusclient(c, 1);

		keyboard = wlr_seat_get_keyboard(seat);
		mods = keyboard ? wlr_keyboard_get_modifiers(keyboard) : 0;
		for (b = buttons; b < buttons + nbuttons; b++) {
			if (CLEANMASK(mods) == CLEANMASK(b->mod) &&
					event->button == b->button && b->func) {
				b->func(&b->arg);
				return;
			}
		}
		break;
	case WL_POINTER_BUTTON_STATE_RELEASED:
		/* Screenshot selection intercepts release too */
		if (screenshot_mode >= SCREENSHOT_SELECTING) {
			screenshot_handle_button(event->button, event->state, event->time_msec);
			return;
		}
		/* Monitor setup popup release (drag drop) */
		{
			Monitor *ms_mon = monitor_setup_visible_monitor();
			if (ms_mon) {
				int cx = (int)lround(cursor->x);
				int cy = (int)lround(cursor->y);
				monitor_setup_handle_button(ms_mon, cx, cy,
					event->button, event->state);
				return;
			}
		}
		/* If you released any buttons, we exit interactive move/resize mode. */
		/* TODO: should reset to the pointer focus's current setcursor */
		if (!locked && cursor_mode != CurNormal && cursor_mode != CurPressed) {
			c = grabc;
			if (c && c->was_tiled && !strcmp(selmon->ltsymbol, "|w|")) {
				if (cursor_mode == CurMove && c->isfloating) {
					target = xytoclient(cursor->x, cursor->y);
					int was_alone = drag_was_alone_in_column;
					int move_allowed;

					/* Clear drag state before modifying tree */
					end_tile_drag();

					/* Restore tiled state */
					c->isfloating = 0;

					/* Column swap takes priority - check if source was alone and target is in different column */
					if (was_alone && target && target != c &&
					    !target->isfloating && !target->isfullscreen &&
					    !same_column(selmon, c, target)) {
						/* Swap columns if dragged tile was alone in its column */
						swap_columns(selmon, c, target);
						arrange(selmon);
					} else {
						/* Check if movement is allowed before modifying anything */
						if (target && target != c && !target->isfloating && !target->isfullscreen)
							move_allowed = can_move_tile(selmon, c, target);
						else
							move_allowed = can_move_tile(selmon, c, NULL);

						if (move_allowed == 0) {
							/* Movement blocked (4 tiles in source or target column) */
							/* Just restore to original position, no change */
							arrange(selmon);
						} else if (move_allowed == 2 && target && target != c) {
							/* Same column swap */
							swap_tiles_in_tree(selmon, c, target);
							arrange(selmon);
						} else {
							/* Remove from old position and insert at new */
							remove_client(selmon, c);
							if (target && target != c && !target->isfloating && !target->isfullscreen)
								insert_client_at(selmon, target, c, cursor->x, cursor->y);
							else
								insert_client(selmon, NULL, c);
							arrange(selmon);
						}
					}

				} else if (cursor_mode == CurResize && !c->isfloating) {
					resizing_from_mouse = 0;
				}
			} else {
				if (cursor_mode == CurResize && resizing_from_mouse)
					resizing_from_mouse = 0;
				resize_last_time = 0;
				end_tile_drag();
			}
			/* Default behaviour */
			nixly_cursor_set_xcursor("default");
			cursor_mode = CurNormal;
			/* Drop the window off on its new monitor */
			selmon = xytomon(cursor->x, cursor->y);
			setmon(grabc, selmon, 0);
			grabc = NULL;
			return;
		}
		cursor_mode = CurNormal;
		break;
	}
	/* If the event wasn't handled by the compositor, notify the client with
	 * pointer focus that a button press has occurred */
	{
		Client *fc = NULL;
		struct wlr_surface *fs = seat->pointer_state.focused_surface;
		if (fs)
			toplevel_from_wlr_surface(fs, &fc, NULL);
		if (fc && event->state == WL_POINTER_BUTTON_STATE_PRESSED &&
				(looks_like_game(fc) || is_game_content(fc)))
			game_log("CURSOR_BUTTON: btn=%u appid='%s' pos=%.0f,%.0f "
				"constraint=%s",
				event->button,
				client_get_appid(fc) ? client_get_appid(fc) : "(null)",
				cursor->x, cursor->y,
				active_constraint ? "active" : "none");
	}
	wlr_seat_pointer_notify_button(seat,
			event->time_msec, event->button, event->state);
}

void
chvt(const Arg *arg)
{
	wlr_session_change_vt(session, arg->ui);
}

void
createkeyboard(struct wlr_keyboard *keyboard)
{
	/* Set the keymap to match the group keymap */
	wlr_keyboard_set_keymap(keyboard, kb_group->wlr_group->keyboard.keymap);

	/* Add the new keyboard to the group */
	wlr_keyboard_group_add_keyboard(kb_group->wlr_group, keyboard);
}

KeyboardGroup *
createkeyboardgroup(void)
{
	KeyboardGroup *group = ecalloc(1, sizeof(*group));
	struct xkb_context *context;
	struct xkb_keymap *keymap;
	struct xkb_rule_names names;

	group->wlr_group = wlr_keyboard_group_create();
	group->wlr_group->data = group;

	/* Prepare an XKB keymap and assign it to the keyboard group. */
	context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	names = getxkbrules();
	if (!(keymap = xkb_keymap_new_from_names(context, &names,
				XKB_KEYMAP_COMPILE_NO_FLAGS)))
		die("failed to compile keymap");

	wlr_keyboard_set_keymap(&group->wlr_group->keyboard, keymap);
	xkb_keymap_unref(keymap);
	xkb_context_unref(context);

	wlr_keyboard_set_repeat_info(&group->wlr_group->keyboard, repeat_rate, repeat_delay);

	/* Set up listeners for keyboard events */
	LISTEN(&group->wlr_group->keyboard.events.key, &group->key, keypress);
	LISTEN(&group->wlr_group->keyboard.events.modifiers, &group->modifiers, keypressmod);

	group->key_repeat_source = wl_event_loop_add_timer(event_loop, keyrepeat, group);

	/* A seat can only have one keyboard, but this is a limitation of the
	 * Wayland protocol - not wlroots. We assign all connected keyboards to the
	 * same wlr_keyboard_group, which provides a single wlr_keyboard interface for
	 * all of them. Set this combined wlr_keyboard as the seat keyboard.
	 */
	wlr_seat_set_keyboard(seat, &group->wlr_group->keyboard);
	return group;
}

/* ── pointer gesture forwarding ─────────────────────────────────────── */

typedef struct {
	struct wl_listener swipe_begin;
	struct wl_listener swipe_update;
	struct wl_listener swipe_end;
	struct wl_listener pinch_begin;
	struct wl_listener pinch_update;
	struct wl_listener pinch_end;
	struct wl_listener hold_begin;
	struct wl_listener hold_end;
	struct wl_listener destroy;
} GestureListeners;

static void gesture_swipe_begin(struct wl_listener *listener, void *data)
{
	struct wlr_pointer_swipe_begin_event *event = data;
	wlr_pointer_gestures_v1_send_swipe_begin(pointer_gestures, seat,
		event->time_msec, event->fingers);
}

static void gesture_swipe_update(struct wl_listener *listener, void *data)
{
	struct wlr_pointer_swipe_update_event *event = data;
	wlr_pointer_gestures_v1_send_swipe_update(pointer_gestures, seat,
		event->time_msec, event->dx, event->dy);
}

static void gesture_swipe_end(struct wl_listener *listener, void *data)
{
	struct wlr_pointer_swipe_end_event *event = data;
	wlr_pointer_gestures_v1_send_swipe_end(pointer_gestures, seat,
		event->time_msec, event->cancelled);
}

static void gesture_pinch_begin(struct wl_listener *listener, void *data)
{
	struct wlr_pointer_pinch_begin_event *event = data;
	wlr_pointer_gestures_v1_send_pinch_begin(pointer_gestures, seat,
		event->time_msec, event->fingers);
}

static void gesture_pinch_update(struct wl_listener *listener, void *data)
{
	struct wlr_pointer_pinch_update_event *event = data;
	wlr_pointer_gestures_v1_send_pinch_update(pointer_gestures, seat,
		event->time_msec, event->dx, event->dy, event->scale, event->rotation);
}

static void gesture_pinch_end(struct wl_listener *listener, void *data)
{
	struct wlr_pointer_pinch_end_event *event = data;
	wlr_pointer_gestures_v1_send_pinch_end(pointer_gestures, seat,
		event->time_msec, event->cancelled);
}

static void gesture_hold_begin(struct wl_listener *listener, void *data)
{
	struct wlr_pointer_hold_begin_event *event = data;
	wlr_pointer_gestures_v1_send_hold_begin(pointer_gestures, seat,
		event->time_msec, event->fingers);
}

static void gesture_hold_end(struct wl_listener *listener, void *data)
{
	struct wlr_pointer_hold_end_event *event = data;
	wlr_pointer_gestures_v1_send_hold_end(pointer_gestures, seat,
		event->time_msec, event->cancelled);
}

static void gesture_listeners_destroy(struct wl_listener *listener, void *data)
{
	GestureListeners *gl = wl_container_of(listener, gl, destroy);
	wl_list_remove(&gl->swipe_begin.link);
	wl_list_remove(&gl->swipe_update.link);
	wl_list_remove(&gl->swipe_end.link);
	wl_list_remove(&gl->pinch_begin.link);
	wl_list_remove(&gl->pinch_update.link);
	wl_list_remove(&gl->pinch_end.link);
	wl_list_remove(&gl->hold_begin.link);
	wl_list_remove(&gl->hold_end.link);
	wl_list_remove(&gl->destroy.link);
	free(gl);
}

void
createpointer(struct wlr_pointer *pointer)
{
	struct libinput_device *device;
	if (wlr_input_device_is_libinput(&pointer->base)
			&& (device = wlr_libinput_get_device_handle(&pointer->base))) {

		if (libinput_device_config_tap_get_finger_count(device)) {
			libinput_device_config_tap_set_enabled(device, tap_to_click);
			libinput_device_config_tap_set_drag_enabled(device, tap_and_drag);
			libinput_device_config_tap_set_drag_lock_enabled(device, drag_lock);
			libinput_device_config_tap_set_button_map(device, button_map);
		}

		if (libinput_device_config_scroll_has_natural_scroll(device))
			libinput_device_config_scroll_set_natural_scroll_enabled(device, natural_scrolling);

		if (libinput_device_config_dwt_is_available(device))
			libinput_device_config_dwt_set_enabled(device, disable_while_typing);

		if (libinput_device_config_left_handed_is_available(device))
			libinput_device_config_left_handed_set(device, left_handed);

		if (libinput_device_config_middle_emulation_is_available(device))
			libinput_device_config_middle_emulation_set_enabled(device, middle_button_emulation);

		if (libinput_device_config_scroll_get_methods(device) != LIBINPUT_CONFIG_SCROLL_NO_SCROLL)
			libinput_device_config_scroll_set_method(device, scroll_method);

		if (libinput_device_config_click_get_methods(device) != LIBINPUT_CONFIG_CLICK_METHOD_NONE)
			libinput_device_config_click_set_method(device, click_method);

		if (libinput_device_config_send_events_get_modes(device))
			libinput_device_config_send_events_set_mode(device, send_events_mode);

		if (libinput_device_config_accel_is_available(device)) {
			libinput_device_config_accel_set_profile(device, accel_profile);
			libinput_device_config_accel_set_speed(device, accel_speed);
		}

		/* Track device for raw input toggle in game mode */
		if (device && pointer_device_count < MAX_POINTER_DEVICES)
			pointer_devices[pointer_device_count++] = device;
	}

	wlr_cursor_attach_input_device(cursor, &pointer->base);

	/* Attach gesture listeners for touchpad swipe/pinch/hold forwarding */
	{
		GestureListeners *gl = ecalloc(1, sizeof(*gl));
		gl->swipe_begin.notify = gesture_swipe_begin;
		gl->swipe_update.notify = gesture_swipe_update;
		gl->swipe_end.notify = gesture_swipe_end;
		gl->pinch_begin.notify = gesture_pinch_begin;
		gl->pinch_update.notify = gesture_pinch_update;
		gl->pinch_end.notify = gesture_pinch_end;
		gl->hold_begin.notify = gesture_hold_begin;
		gl->hold_end.notify = gesture_hold_end;
		gl->destroy.notify = gesture_listeners_destroy;
		wl_signal_add(&pointer->events.swipe_begin, &gl->swipe_begin);
		wl_signal_add(&pointer->events.swipe_update, &gl->swipe_update);
		wl_signal_add(&pointer->events.swipe_end, &gl->swipe_end);
		wl_signal_add(&pointer->events.pinch_begin, &gl->pinch_begin);
		wl_signal_add(&pointer->events.pinch_update, &gl->pinch_update);
		wl_signal_add(&pointer->events.pinch_end, &gl->pinch_end);
		wl_signal_add(&pointer->events.hold_begin, &gl->hold_begin);
		wl_signal_add(&pointer->events.hold_end, &gl->hold_end);
		wl_signal_add(&pointer->base.events.destroy, &gl->destroy);
	}
}

void
newkbshortcutsinhibitor(struct wl_listener *listener, void *data)
{
	struct wlr_keyboard_shortcuts_inhibitor_v1 *inhibitor = data;

	/* Always honor the inhibitor — fullscreen games, remote desktop, VMs
	 * need to capture compositor shortcuts like Alt+Tab. The inhibitor is
	 * only active while the requesting surface has keyboard focus. */
	wlr_keyboard_shortcuts_inhibitor_v1_activate(inhibitor);
}

void
cursorwarptohint(void)
{
	Client *c = NULL;
	double sx = active_constraint->current.cursor_hint.x;
	double sy = active_constraint->current.cursor_hint.y;

	toplevel_from_wlr_surface(active_constraint->surface, &c, NULL);
	if (c && active_constraint->current.cursor_hint.enabled) {
		double off_x = c->scene_surface ? c->scene_surface->node.x : c->bw;
		double off_y = c->scene_surface ? c->scene_surface->node.y : c->bw;
		wlr_cursor_warp(cursor, NULL, sx + c->geom.x + off_x, sy + c->geom.y + off_y);
		wlr_seat_pointer_warp(active_constraint->seat, sx, sy);
	}
}

void
checkconstraint(void)
{
	struct wlr_pointer_constraint_v1 *constraint;
	struct wlr_surface *focused = seat->pointer_state.focused_surface;

	/* Already tracking the right constraint */
	if (active_constraint && active_constraint->surface == focused)
		return;

	/* Deactivate stale constraint (surface lost focus) */
	if (active_constraint) {
		Client *dc = NULL;
		toplevel_from_wlr_surface(active_constraint->surface, &dc, NULL);
		if (dc && (looks_like_game(dc) || is_game_content(dc)))
			game_log("CURSOR_CONSTRAINT: DEACTIVATE type=%s appid='%s'",
				active_constraint->type == WLR_POINTER_CONSTRAINT_V1_LOCKED
					? "lock" : "confine",
				client_get_appid(dc) ? client_get_appid(dc) : "(null)");
		cursorwarptohint();
		wlr_pointer_constraint_v1_send_deactivated(active_constraint);
		active_constraint = NULL;
	}

	/* Find and activate constraint for the focused surface */
	if (focused) {
		wl_list_for_each(constraint, &pointer_constraints->constraints, link) {
			if (constraint->surface == focused) {
				active_constraint = constraint;
				wlr_pointer_constraint_v1_send_activated(constraint);
				Client *ac = NULL;
				toplevel_from_wlr_surface(focused, &ac, NULL);
				if (ac && (looks_like_game(ac) || is_game_content(ac)))
					game_log("CURSOR_CONSTRAINT: ACTIVATE type=%s appid='%s'",
						constraint->type == WLR_POINTER_CONSTRAINT_V1_LOCKED
							? "lock" : "confine",
						client_get_appid(ac) ? client_get_appid(ac) : "(null)");
				return;
			}
		}
	}
}

void
createpointerconstraint(struct wl_listener *listener, void *data)
{
	struct wlr_pointer_constraint_v1 *wlr_constraint = data;
	PointerConstraint *pointer_constraint = ecalloc(1, sizeof(*pointer_constraint));
	pointer_constraint->constraint = wlr_constraint;
	LISTEN(&wlr_constraint->events.destroy,
			&pointer_constraint->destroy, destroypointerconstraint);

	/*
	 * If the constraint's surface belongs to a mapped, fullscreen client on
	 * the monitor where the cursor currently is, force pointer-focus to that
	 * client so checkconstraint() can activate. This handles the late-creation
	 * race for XWayland games where the constraint is registered after
	 * focusclient/motionnotify already ran.
	 */
	Client *c = NULL;
	toplevel_from_wlr_surface(wlr_constraint->surface, &c, NULL);

	if (c && (looks_like_game(c) || is_game_content(c)))
		game_log("CURSOR_CONSTRAINT: CREATE type=%s appid='%s' "
			"fullscreen=%d late_activate=%d",
			wlr_constraint->type == WLR_POINTER_CONSTRAINT_V1_LOCKED
				? "lock" : "confine",
			client_get_appid(c) ? client_get_appid(c) : "(null)",
			c->isfullscreen,
			(c->mon && client_surface(c) && client_surface(c)->mapped
				&& xytomon(cursor->x, cursor->y) == c->mon
				&& c->isfullscreen) ? 1 : 0);

	if (c && c->mon && client_surface(c) && client_surface(c)->mapped) {
		Monitor *cm = xytomon(cursor->x, cursor->y);
		if (cm == c->mon && c->isfullscreen) {
			/* Re-run pointer focus at current cursor position so
			 * focused_surface becomes the game's surface. */
			motionnotify(0, NULL, 0, 0, 0, 0);
		}
	}

	checkconstraint();
}

void
cursorframe(struct wl_listener *listener, void *data)
{
	/* This event is forwarded by the cursor when a pointer emits a frame
	 * event. Frame events are sent after regular pointer events to group
	 * multiple events together. For instance, two axis events may happen at the
	 * same time, in which case a frame event won't be sent in between. */
	/* Notify the client with pointer focus of the frame event. */
	wlr_seat_pointer_notify_frame(seat);
}

void
destroydragicon(struct wl_listener *listener, void *data)
{
	/* Focus enter isn't sent during drag, so refocus the focused node. */
	focusclient(focustop(selmon), 1);
	motionnotify(0, NULL, 0, 0, 0, 0);
	wl_list_remove(&listener->link);
	free(listener);
}

void
destroypointerconstraint(struct wl_listener *listener, void *data)
{
	PointerConstraint *pointer_constraint = wl_container_of(listener, pointer_constraint, destroy);

	if (active_constraint == pointer_constraint->constraint) {
		cursorwarptohint();
		active_constraint = NULL;
	}

	wl_list_remove(&pointer_constraint->destroy.link);
	free(pointer_constraint);
}

void
destroykeyboardgroup(struct wl_listener *listener, void *data)
{
	KeyboardGroup *group = wl_container_of(listener, group, destroy);
	wl_event_source_remove(group->key_repeat_source);
	wl_list_remove(&group->key.link);
	wl_list_remove(&group->modifiers.link);
	wl_list_remove(&group->destroy.link);
	wlr_keyboard_group_destroy(group->wlr_group);
	free(group);
}

/* ── tablet (pen / pad) ──────────────────────────────────────────── */

static void
tablettoolsetcursor(struct wl_listener *listener, void *data)
{
	struct wlr_tablet_v2_event_cursor *event = data;
	if (cursor_mode != CurNormal && cursor_mode != CurPressed)
		return;
	if (event->seat_client == seat->pointer_state.focused_client)
		nixly_cursor_set_client_surface(event->surface,
				event->hotspot_x, event->hotspot_y);
}

static struct wlr_tablet_v2_tablet_tool *
get_or_create_tool(struct wlr_tablet_tool *tool)
{
	struct wlr_tablet_v2_tablet_tool *v2 = tool->data;
	if (v2)
		return v2;
	v2 = wlr_tablet_tool_create(tablet_v2_mgr, seat, tool);
	if (!v2)
		return NULL;
	tool->data = v2;
	LISTEN_STATIC(&v2->events.set_cursor, tablettoolsetcursor);
	return v2;
}

static void
tabletaxis(struct wl_listener *listener, void *data)
{
	struct wlr_tablet_tool_axis_event *ev = data;
	struct wlr_tablet_v2_tablet *v2tab = ev->tablet->data;
	struct wlr_tablet_v2_tablet_tool *v2tool = get_or_create_tool(ev->tool);
	if (!v2tab || !v2tool)
		return;

	if (ev->updated_axes & (WLR_TABLET_TOOL_AXIS_X | WLR_TABLET_TOOL_AXIS_Y))
		wlr_cursor_warp_absolute(cursor, &ev->tablet->base,
				(ev->updated_axes & WLR_TABLET_TOOL_AXIS_X) ? ev->x : NAN,
				(ev->updated_axes & WLR_TABLET_TOOL_AXIS_Y) ? ev->y : NAN);

	struct wlr_surface *surface = NULL;
	Client *c = NULL;
	double sx, sy;
	xytonode(cursor->x, cursor->y, &surface, &c, NULL, &sx, &sy);

	if (surface && surface != v2tool->focused_surface) {
		wlr_send_tablet_v2_tablet_tool_proximity_out(v2tool);
		wlr_send_tablet_v2_tablet_tool_proximity_in(v2tool, v2tab, surface);
	}

	if (surface) {
		wlr_send_tablet_v2_tablet_tool_motion(v2tool, sx, sy);

		if (ev->updated_axes & WLR_TABLET_TOOL_AXIS_PRESSURE)
			wlr_send_tablet_v2_tablet_tool_pressure(v2tool, ev->pressure);
		if (ev->updated_axes & WLR_TABLET_TOOL_AXIS_DISTANCE)
			wlr_send_tablet_v2_tablet_tool_distance(v2tool, ev->distance);
		if (ev->updated_axes & (WLR_TABLET_TOOL_AXIS_TILT_X | WLR_TABLET_TOOL_AXIS_TILT_Y))
			wlr_send_tablet_v2_tablet_tool_tilt(v2tool, ev->tilt_x, ev->tilt_y);
		if (ev->updated_axes & WLR_TABLET_TOOL_AXIS_ROTATION)
			wlr_send_tablet_v2_tablet_tool_rotation(v2tool, ev->rotation);
		if (ev->updated_axes & WLR_TABLET_TOOL_AXIS_SLIDER)
			wlr_send_tablet_v2_tablet_tool_slider(v2tool, ev->slider);
		if (ev->updated_axes & WLR_TABLET_TOOL_AXIS_WHEEL)
			wlr_send_tablet_v2_tablet_tool_wheel(v2tool, ev->wheel_delta, 0);
	}
}

static void
tabletproximity(struct wl_listener *listener, void *data)
{
	struct wlr_tablet_tool_proximity_event *ev = data;
	struct wlr_tablet_v2_tablet *v2tab = ev->tablet->data;
	struct wlr_tablet_v2_tablet_tool *v2tool = get_or_create_tool(ev->tool);
	if (!v2tab || !v2tool)
		return;

	if (ev->state == WLR_TABLET_TOOL_PROXIMITY_IN) {
		wlr_cursor_warp_absolute(cursor, &ev->tablet->base, ev->x, ev->y);

		struct wlr_surface *surface = NULL;
		double sx, sy;
		xytonode(cursor->x, cursor->y, &surface, NULL, NULL, &sx, &sy);

		if (surface)
			wlr_send_tablet_v2_tablet_tool_proximity_in(v2tool, v2tab, surface);
	} else {
		wlr_send_tablet_v2_tablet_tool_proximity_out(v2tool);
	}
}

static void
tablettip(struct wl_listener *listener, void *data)
{
	struct wlr_tablet_tool_tip_event *ev = data;
	struct wlr_tablet_v2_tablet_tool *v2tool = get_or_create_tool(ev->tool);
	if (!v2tool)
		return;

	if (ev->state == WLR_TABLET_TOOL_TIP_DOWN) {
		wlr_send_tablet_v2_tablet_tool_down(v2tool);
		wlr_tablet_tool_v2_start_implicit_grab(v2tool);
	} else {
		wlr_send_tablet_v2_tablet_tool_up(v2tool);
	}
}

static void
tabletbutton(struct wl_listener *listener, void *data)
{
	struct wlr_tablet_tool_button_event *ev = data;
	struct wlr_tablet_v2_tablet_tool *v2tool = get_or_create_tool(ev->tool);
	if (!v2tool)
		return;

	wlr_send_tablet_v2_tablet_tool_button(v2tool, ev->button,
			(enum zwp_tablet_pad_v2_button_state)ev->state);
}

static void
tabletpadbutton(struct wl_listener *listener, void *data)
{
	struct wlr_tablet_pad_button_event *ev = data;
	TabletPad *tp = wl_container_of(listener, tp, button);
	wlr_send_tablet_v2_tablet_pad_button(tp->v2, ev->button,
			ev->time_msec,
			(enum zwp_tablet_pad_v2_button_state)ev->state);
}

static void
tabletpadring(struct wl_listener *listener, void *data)
{
	struct wlr_tablet_pad_ring_event *ev = data;
	TabletPad *tp = wl_container_of(listener, tp, ring);
	wlr_send_tablet_v2_tablet_pad_ring(tp->v2, ev->ring, ev->position,
			ev->source == WLR_TABLET_PAD_RING_SOURCE_FINGER,
			ev->time_msec);
}

static void
tabletpadstrip(struct wl_listener *listener, void *data)
{
	struct wlr_tablet_pad_strip_event *ev = data;
	TabletPad *tp = wl_container_of(listener, tp, strip);
	wlr_send_tablet_v2_tablet_pad_strip(tp->v2, ev->strip, ev->position,
			ev->source == WLR_TABLET_PAD_STRIP_SOURCE_FINGER,
			ev->time_msec);
}

static void
tabletpaddestroy(struct wl_listener *listener, void *data)
{
	TabletPad *tp = wl_container_of(listener, tp, destroy);
	wl_list_remove(&tp->button.link);
	wl_list_remove(&tp->ring.link);
	wl_list_remove(&tp->strip.link);
	wl_list_remove(&tp->destroy.link);
	free(tp);
}

static void
createtablet(struct wlr_tablet *tablet)
{
	struct wlr_tablet_v2_tablet *v2 =
		wlr_tablet_create(tablet_v2_mgr, seat, &tablet->base);
	if (!v2)
		return;
	tablet->data = v2;
	wlr_cursor_attach_input_device(cursor, &tablet->base);
	LISTEN_STATIC(&tablet->events.axis, tabletaxis);
	LISTEN_STATIC(&tablet->events.proximity, tabletproximity);
	LISTEN_STATIC(&tablet->events.tip, tablettip);
	LISTEN_STATIC(&tablet->events.button, tabletbutton);
}

static void
createtabletpad(struct wlr_tablet_pad *pad)
{
	struct wlr_tablet_v2_tablet_pad *v2 =
		wlr_tablet_pad_create(tablet_v2_mgr, seat, &pad->base);
	if (!v2)
		return;
	TabletPad *tp = ecalloc(1, sizeof(*tp));
	tp->pad = pad;
	tp->v2 = v2;
	LISTEN(&pad->events.button, &tp->button, tabletpadbutton);
	LISTEN(&pad->events.ring, &tp->ring, tabletpadring);
	LISTEN(&pad->events.strip, &tp->strip, tabletpadstrip);
	LISTEN(&pad->base.events.destroy, &tp->destroy, tabletpaddestroy);
}

void
inputdevice(struct wl_listener *listener, void *data)
{
	/* This event is raised by the backend when a new input device becomes
	 * available. */
	struct wlr_input_device *device = data;
	uint32_t caps;

	switch (device->type) {
	case WLR_INPUT_DEVICE_KEYBOARD:
		createkeyboard(wlr_keyboard_from_input_device(device));
		break;
	case WLR_INPUT_DEVICE_POINTER:
		createpointer(wlr_pointer_from_input_device(device));
		break;
	case WLR_INPUT_DEVICE_TABLET:
		createtablet(wlr_tablet_from_input_device(device));
		break;
	case WLR_INPUT_DEVICE_TABLET_PAD:
		createtabletpad(wlr_tablet_pad_from_input_device(device));
		break;
	default:
		break;
	}

	/* We need to let the wlr_seat know what our capabilities are, which is
	 * communiciated to the client. In dwl we always have a cursor, even if
	 * there are no pointer devices, so we always include that capability. */
	/* TODO do we actually require a cursor? */
	caps = WL_SEAT_CAPABILITY_POINTER;
	if (!wl_list_empty(&kb_group->wlr_group->devices))
		caps |= WL_SEAT_CAPABILITY_KEYBOARD;
	wlr_seat_set_capabilities(seat, caps);
}

int
keybinding(uint32_t mods, xkb_keysym_t sym)
{
	/*
	 * Here we handle compositor keybindings. This is when the compositor is
	 * processing keys, rather than passing them on to the client for its own
	 * processing.
	 */
	size_t i;
	for (i = 0; i < keys_count; i++) {
		const Key *k = &keys[i];
		if (CLEANMASK(mods) == CLEANMASK(k->mod)
				&& sym == k->keysym && k->func) {
			last_keybinding_func = k->func;
			k->func(&k->arg);
			return 1;
		}
	}
	return 0;
}

static int
shortcuts_are_inhibited(void)
{
	struct wlr_keyboard_shortcuts_inhibitor_v1 *inhibitor;
	wl_list_for_each(inhibitor, &kb_shortcuts_inhibit_mgr->inhibitors, link) {
		if (inhibitor->active &&
		    inhibitor->surface == seat->keyboard_state.focused_surface)
			return 1;
	}
	return 0;
}

static int
try_screenshot_key(const xkb_keysym_t *syms, int nsyms)
{
	if (!screenshot_mode)
		return 0;
	for (int i = 0; i < nsyms; i++) {
		if (syms[i] == XKB_KEY_Escape) {
			screenshot_handle_key(syms[i]);
			return 1;
		}
	}
	return 0;
}

static int
try_monitor_setup_key(const xkb_keysym_t *syms, int nsyms)
{
	Monitor *ms_mon = monitor_setup_visible_monitor();
	if (!ms_mon)
		return 0;
	for (int i = 0; i < nsyms; i++) {
		if (monitor_setup_handle_key(ms_mon, syms[i]))
			return 1;
	}
	return 0;
}

static int
try_sudo_popup_key(Monitor *sudo_mon, const xkb_keysym_t *syms, int nsyms, uint32_t mods)
{
	if (!sudo_mon)
		return 0;
	for (int i = 0; i < nsyms; i++) {
		if (sudo_popup_handle_key(sudo_mon, mods, syms[i]))
			return 1;
	}
	return 0;
}

static int
try_wifi_popup_key(Monitor *wifi_mon, const xkb_keysym_t *syms, int nsyms, uint32_t mods)
{
	if (!wifi_mon)
		return 0;
	for (int i = 0; i < nsyms; i++) {
		if (wifi_popup_handle_key(wifi_mon, mods, syms[i]))
			return 1;
	}
	return 0;
}

static int
try_stats_panel_key(Monitor *stats_mon, const xkb_keysym_t *syms, int nsyms)
{
	if (!stats_mon)
		return 0;
	for (int i = 0; i < nsyms; i++) {
		if (stats_panel_handle_key(stats_mon, syms[i]))
			return 1;
	}
	return 0;
}


static int
try_nixpkgs_key(Monitor *nixpkgs_mon, const xkb_keysym_t *syms, int nsyms, uint32_t mods)
{
	int consumed = 0, is_navigation = 0;

	if (!nixpkgs_mon)
		return 0;
	for (int i = 0; i < nsyms; i++) {
		xkb_keysym_t s = syms[i];
		if (nixpkgs_handle_key(nixpkgs_mon, mods, s))
			consumed = 1;
		if (s == XKB_KEY_Up || s == XKB_KEY_Down)
			is_navigation = 1;
	}
	if (consumed) {
		if (is_navigation)
			nixpkgs_update_selection(nixpkgs_mon);
		else {
			nixpkgs_update_results(nixpkgs_mon);
			nixpkgs_render(nixpkgs_mon);
		}
	}
	return consumed;
}

static int
try_modal_key(Monitor *modal_mon, const xkb_keysym_t *syms, int nsyms, uint32_t mods)
{
	int consumed = 0, is_text_input = 0;

	if (!modal_mon)
		return 0;
	for (int i = 0; i < nsyms; i++) {
		xkb_keysym_t s = syms[i];
		if ((s >= 0x20 && s <= 0x7e) || s == XKB_KEY_BackSpace)
			is_text_input = 1;
		if (modal_handle_key(modal_mon, mods, s))
			consumed = 1;
	}
	if (consumed) {
		int is_file_search = (modal_mon->modal.active_idx == 1);
		int is_navigation = 0;
		for (int i = 0; i < nsyms; i++) {
			if (syms[i] == XKB_KEY_Up || syms[i] == XKB_KEY_Down) {
				is_navigation = 1;
				break;
			}
		}
		if (is_navigation) {
			modal_update_selection(modal_mon);
		} else if (is_text_input && is_file_search) {
			modal_render_search_field(modal_mon);
			if (!modal_mon->modal.render_timer)
				modal_mon->modal.render_timer = wl_event_loop_add_timer(
					event_loop, modal_render_timer_cb, modal_mon);
			modal_mon->modal.render_pending = 1;
			wl_event_source_timer_update(modal_mon->modal.render_timer, 300);
		} else {
			modal_update_results(modal_mon);
			modal_render(modal_mon);
		}
	}
	return consumed;
}

void
keypress(struct wl_listener *listener, void *data)
{
	int i;
	/* This event is raised when a key is pressed or released. */
	KeyboardGroup *group = wl_container_of(listener, group, key);
	struct wlr_keyboard_key_event *event = data;

	/*
	 * Track input timestamp for latency measurement.
	 * This helps measure input-to-photon latency in games.
	 */
	if (game_mode_ultra && selmon) {
		selmon->last_input_ns = get_time_ns();
	}

	/* Translate libinput keycode -> xkbcommon */
	uint32_t keycode = event->keycode + 8;
	/* Get a list of keysyms based on the keymap for this keyboard */
	const xkb_keysym_t *syms;
	int nsyms = xkb_state_key_get_syms(
			group->wlr_group->keyboard.xkb_state, keycode, &syms);
	/* Also get the base keysym (level 0, no modifiers applied) for shifted bindings */
	struct xkb_keymap *keymap = group->wlr_group->keyboard.keymap;
	xkb_layout_index_t layout = xkb_state_key_get_layout(
			group->wlr_group->keyboard.xkb_state, keycode);
	const xkb_keysym_t *level0_syms;
	int nlevel0 = xkb_keymap_key_get_syms_by_level(keymap, keycode, layout, 0, &level0_syms);

	int handled = 0;
	uint32_t mods = wlr_keyboard_get_modifiers(&group->wlr_group->keyboard);
	Monitor *modal_mon = modal_visible_monitor();
	Monitor *nixpkgs_mon = nixpkgs_visible_monitor();
	Monitor *wifi_mon = wifi_popup_visible_monitor();
	Monitor *sudo_mon = sudo_popup_visible_monitor();
	Monitor *stats_mon = stats_panel_visible_monitor();

	wlr_idle_notifier_v1_notify_activity(idle_notifier, seat);

	/* VT switching using raw evdev keycodes — always works regardless of
	 * XKB keymap, screen lock state, or active popups/overlays. */
	if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED && session &&
	    (mods & (WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT)) ==
	    (WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT)) {
		unsigned int vt = 0;
		if (event->keycode >= KEY_F1 && event->keycode <= KEY_F10)
			vt = event->keycode - KEY_F1 + 1;
		else if (event->keycode == KEY_F11)
			vt = 11;
		else if (event->keycode == KEY_F12)
			vt = 12;
		if (vt) {
			wlr_session_change_vt(session, vt);
			return;
		}
	}

	/* On _press_ if there is no active screen locker,
	 * attempt to process a compositor keybinding. */
	if (!locked && event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		handled = try_screenshot_key(syms, nsyms)
		       || try_monitor_setup_key(syms, nsyms)
		       || try_sudo_popup_key(sudo_mon, syms, nsyms, mods)
		       || try_wifi_popup_key(wifi_mon, syms, nsyms, mods)
		       || try_stats_panel_key(stats_mon, syms, nsyms)
		       || try_nixpkgs_key(nixpkgs_mon, syms, nsyms, mods)
		       || try_modal_key(modal_mon, syms, nsyms, mods);
		if (!handled && !shortcuts_are_inhibited()) {
			last_keybinding_func = NULL;
			for (i = 0; i < nsyms; i++)
				handled = keybinding(mods, syms[i]) || handled;
		}
		if (!handled && nlevel0 > 0 && !shortcuts_are_inhibited()) {
			for (i = 0; i < nlevel0; i++)
				handled = keybinding(mods, level0_syms[i]) || handled;
		}
	}

	/* Don't enable key repeat for modal/nixpkgs text input to avoid double characters.
	 * Only allow repeat for navigation keys (arrows, backspace) in modal/nixpkgs.
	 * Also suppress repeat for toggle actions (e.g. togglestatusbar). */
	{
		int allow_repeat = 0;
		if (handled && group->wlr_group->keyboard.repeat_info.delay > 0) {
			if (!modal_mon && !nixpkgs_mon) {
				allow_repeat = (last_keybinding_func != togglestatusbar);
			} else {
				/* In modal/nixpkgs: only allow repeat for navigation keys */
				for (i = 0; i < nsyms; i++) {
					xkb_keysym_t s = syms[i];
					if (s == XKB_KEY_Up || s == XKB_KEY_Down ||
					    s == XKB_KEY_BackSpace) {
						allow_repeat = 1;
						break;
					}
				}
			}
		}
		if (allow_repeat) {
			group->mods = mods;
			group->keysyms = syms;
			group->nsyms = nsyms;
			wl_event_source_timer_update(group->key_repeat_source,
					group->wlr_group->keyboard.repeat_info.delay);
		} else {
			group->nsyms = 0;
			wl_event_source_timer_update(group->key_repeat_source, 0);
		}
	}

	if (handled)
		return;

	wlr_seat_set_keyboard(seat, &group->wlr_group->keyboard);
	/* Pass unhandled keycodes along to the client. */
	if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		struct wlr_surface *focused = seat->keyboard_state.focused_surface;
		wlr_log(WLR_DEBUG, "keypress: forwarding key %d to client, focused_surface=%p",
			event->keycode, (void*)focused);
	}
	wlr_seat_keyboard_notify_key(seat, event->time_msec,
			event->keycode, event->state);
}

void
keypressmod(struct wl_listener *listener, void *data)
{
	/* This event is raised when a modifier key, such as shift or alt, is
	 * pressed. We simply communicate this to the client. */
	KeyboardGroup *group = wl_container_of(listener, group, modifiers);

	wlr_seat_set_keyboard(seat, &group->wlr_group->keyboard);
	/* Send modifiers to the client. */
	wlr_seat_keyboard_notify_modifiers(seat,
			&group->wlr_group->keyboard.modifiers);
}

int
keyrepeat(void *data)
{
	KeyboardGroup *group = data;
	Monitor *modal_mon = modal_visible_monitor();
	Monitor *nixpkgs_mon = nixpkgs_visible_monitor();
	int modal_consumed = 0;
	int nixpkgs_consumed = 0;
	int i;
	if (!group->nsyms || group->wlr_group->keyboard.repeat_info.rate <= 0)
		return 0;

	wl_event_source_timer_update(group->key_repeat_source,
			1000 / group->wlr_group->keyboard.repeat_info.rate);

	for (i = 0; i < group->nsyms; i++) {
		if (nixpkgs_mon && nixpkgs_handle_key(nixpkgs_mon, group->mods, group->keysyms[i]))
			nixpkgs_consumed = 1;
		else if (modal_mon && modal_handle_key(modal_mon, group->mods, group->keysyms[i]))
			modal_consumed = 1;
		else
			keybinding(group->mods, group->keysyms[i]);
	}

	if (nixpkgs_consumed && nixpkgs_mon) {
		/* Check if this is navigation - use fast path */
		int is_nav = 0;
		for (i = 0; i < group->nsyms; i++) {
			if (group->keysyms[i] == XKB_KEY_Up || group->keysyms[i] == XKB_KEY_Down) {
				is_nav = 1;
				break;
			}
		}
		if (is_nav) {
			/* Ultra-fast: just toggle highlights */
			nixpkgs_update_selection(nixpkgs_mon);
		} else {
			/* Other keys (backspace): full update */
			nixpkgs_update_results(nixpkgs_mon);
			nixpkgs_render(nixpkgs_mon);
		}
	}

	if (modal_consumed && modal_mon) {
		/* Check if this is navigation - use fast path */
		int is_nav = 0;
		for (i = 0; i < group->nsyms; i++) {
			if (group->keysyms[i] == XKB_KEY_Up || group->keysyms[i] == XKB_KEY_Down) {
				is_nav = 1;
				break;
			}
		}
		if (is_nav) {
			/* Ultra-fast: just toggle highlights */
			modal_update_selection(modal_mon);
		} else {
			/* Other keys (backspace): full update */
			modal_update_results(modal_mon);
			modal_render(modal_mon);
		}
	}

	return 0;
}

void
motionabsolute(struct wl_listener *listener, void *data)
{
	/* This event is forwarded by the cursor when a pointer emits an _absolute_
	 * motion event, from 0..1 on each axis. This happens, for example, when
	 * wlroots is running under a Wayland window rather than KMS+DRM, and you
	 * move the mouse over the window. You could enter the window from any edge,
	 * so we have to warp the mouse there. Also, some hardware emits these events. */
	struct wlr_pointer_motion_absolute_event *event = data;
	double lx, ly, dx, dy;

	if (!event->time_msec) /* this is 0 with virtual pointers */
		wlr_cursor_warp_absolute(cursor, &event->pointer->base, event->x, event->y);

	wlr_cursor_absolute_to_layout_coords(cursor, &event->pointer->base, event->x, event->y, &lx, &ly);
	dx = lx - cursor->x;
	dy = ly - cursor->y;
	last_pointer_motion_ms = monotonic_msec();
	motionnotify(event->time_msec, &event->pointer->base, dx, dy, dx, dy);
}

int
subtree_bounds(LayoutNode *node, Monitor *m, struct wlr_box *out)
{
	struct wlr_box lbox, rbox;
	int hasl, hasr, right, bottom;
	struct wlr_box tmp;

	if (!out)
		out = &tmp;
	if (!node)
		return 0;
	if (node->is_client_node) {
		Client *c = node->client;
		if (!c || !VISIBLEON(c, m) || c->isfloating || c->isfullscreen)
			return 0;
		*out = c->geom;
		return 1;
	}

	hasl = subtree_bounds(node->left, m, &lbox);
	hasr = subtree_bounds(node->right, m, &rbox);
	if (!hasl && !hasr)
		return 0;
	if (hasl && !hasr) {
		*out = lbox;
		return 1;
	}
	if (!hasl && hasr) {
		*out = rbox;
		return 1;
	}

	out->x = MIN(lbox.x, rbox.x);
	out->y = MIN(lbox.y, rbox.y);
	right = MAX(lbox.x + lbox.width, rbox.x + rbox.width);
	bottom = MAX(lbox.y + lbox.height, rbox.y + rbox.height);
	out->width = right - out->x;
	out->height = bottom - out->y;
	return 1;
}

/* ancestor_split moved to layout.c */

LayoutNode *
closest_split_node(LayoutNode *client_node, int want_vert, double pointer,
		double *out_ratio, struct wlr_box *out_box, double *out_dist)
{
	LayoutNode *n = client_node ? client_node->split_node : NULL;
	LayoutNode *best = NULL;
	double best_dist = HUGE_VAL;
	struct wlr_box box;

	while (n) {
		if (!n->is_client_node && n->is_split_vertically == (unsigned int)want_vert) {
			if (subtree_bounds(n, selmon, &box)) {
				double ratio = n->split_ratio;
				double div = want_vert ? box.x + box.width * ratio
						: box.y + box.height * ratio;
				double dist = fabs(pointer - div);

				if (dist < best_dist) {
					best_dist = dist;
					best = n;
					if (out_ratio)
						*out_ratio = ratio;
					if (out_box)
						*out_box = box;
				}
			}
		}
		n = n->split_node;
	}

	if (out_dist)
		*out_dist = best ? best_dist : HUGE_VAL;
	return best;
}

void
apply_resize_axis_choice(void)
{
	resize_use_v = resize_split_node != NULL;
	resize_use_h = resize_split_node_h != NULL;
}

int
resize_should_update(uint32_t time)
{
	uint32_t elapsed;
	double dist;

	if (!resizing_from_mouse || time == 0 || resize_interval_ms == 0)
		return 1;

	if (resize_last_time == 0) {
		resize_last_time = time;
		resize_last_x = cursor->x;
		resize_last_y = cursor->y;
		return 1;
	}

	elapsed = time - resize_last_time;
	dist = fabs(cursor->x - resize_last_x) + fabs(cursor->y - resize_last_y);

	if (elapsed < resize_interval_ms && dist < resize_min_pixels)
		return 0;

	resize_last_time = time;
	resize_last_x = cursor->x;
	resize_last_y = cursor->y;
	return 1;
}

const char *
resize_cursor_from_dirs(int dx, int dy)
{
	if (dx == 0 && dy == -1)
		return "n-resize";
	if (dx == 0 && dy == 1)
		return "s-resize";
	if (dx == -1 && dy == 0)
		return "w-resize";
	if (dx == 1 && dy == 0)
		return "e-resize";
	if (dx == -1 && dy == -1)
		return "nw-resize";
	if (dx == 1 && dy == -1)
		return "ne-resize";
	if (dx == -1 && dy == 1)
		return "sw-resize";
	if (dx == 1 && dy == 1)
		return "se-resize";
	return "default";
}

const char *
pick_resize_handle(const Client *c, double cx, double cy)
{
	double left = cx - c->geom.x;
	double right = c->geom.x + c->geom.width - cx;
	double top = cy - c->geom.y;
	double bottom = c->geom.y + c->geom.height - cy;
	double corner_thresh = MIN(24.0, MIN(c->geom.width, c->geom.height) / 3.0);
	int hx = (left <= right) ? -1 : 1;
	int hy = (top <= bottom) ? -1 : 1;

	resize_dir_x = 0;
	resize_dir_y = 0;
	resize_use_v = resize_use_h = 0;

	if (MIN(left, right) <= corner_thresh && MIN(top, bottom) <= corner_thresh) {
		/* Close to a corner: resize both axes. */
		resize_dir_x = hx;
		resize_dir_y = hy;
	} else {
		if (MIN(left, right) <= corner_thresh)
			resize_dir_x = hx;
		if (MIN(top, bottom) <= corner_thresh)
			resize_dir_y = hy;
		if (!resize_dir_x && !resize_dir_y) {
			/* Otherwise pick the nearest axis so resize still happens. */
			if (MIN(left, right) <= MIN(top, bottom))
				resize_dir_x = hx;
			else
				resize_dir_y = hy;
		}
	}

	return resize_cursor_from_dirs(resize_dir_x, resize_dir_y);
}

/*
 * Handle cursor motion that crosses between monitors of different geometries.
 *
 * Three situations are addressed:
 *
 *   1. Horizontal crossing between monitors of different heights.
 *      Y is remapped proportionally so the cursor enters the destination
 *      monitor at the same relative vertical position — top of source maps
 *      to top of destination, bottom of source maps to bottom of destination.
 *      This makes side-by-side monitors of unequal height feel as if they
 *      share the same height ("virtual equal-height" behaviour).
 *
 *   2. Vertical crossing between monitors of different widths, or off an
 *      edge with no monitor in that direction.
 *      When a monitor exists in the vertical direction, X is remapped
 *      proportionally (symmetric to case 1).
 *      When no monitor exists above/below the current one at the cursor's
 *      X position, the cursor is clamped to the current monitor's top or
 *      bottom edge — i.e. the bottom blocks the cursor the same way the
 *      top does, unless a monitor is placed below in the config.
 *
 *   3. Diagonal motion that leaves the current monitor into a dead zone
 *      (a corner gap between misaligned monitors).
 *      Fallback: snap to the nearest valid edge of a neighbouring monitor
 *      in the dominant direction of motion. Preserves the previous
 *      behaviour for corner diagonals.
 *
 * Returns 1 when the cursor was warped directly — caller must skip
 * wlr_cursor_move(). Returns 0 to let the default motion path handle it.
 */
static int
cursor_snap_across_dead_zone(double dx, double dy)
{
	if (dx == 0 && dy == 0)
		return 0;

	double tx = cursor->x + dx;
	double ty = cursor->y + dy;

	Monitor *cur_mon = xytomon(cursor->x, cursor->y);
	if (!cur_mon)
		return 0;
	struct wlr_box *sb = &cur_mon->m;
	if (sb->width <= 0 || sb->height <= 0)
		return 0;

	int crosses_h = (dx > 0 && tx >= sb->x + sb->width) ||
			(dx < 0 && tx < sb->x);
	int crosses_v = (dy > 0 && ty >= sb->y + sb->height) ||
			(dy < 0 && ty < sb->y);

	/* When both axes cross simultaneously (diagonal past a corner), decide
	 * which axis wins by the dominant motion component. */
	int prefer_h = crosses_h && (!crosses_v || fabs(dx) >= fabs(dy));

	/* CASE 1: horizontal-dominant crossing between monitors.
	 * Find the nearest monitor in the horizontal direction of motion and
	 * warp with proportional Y remapping. */
	if (prefer_h) {
		Monitor *best = NULL;
		double best_gap = 1e30;
		Monitor *m;
		wl_list_for_each(m, &mons, link) {
			if (!m->wlr_output->enabled || m->is_mirror || m == cur_mon)
				continue;
			struct wlr_box *db = &m->m;
			if (db->width <= 0 || db->height <= 0)
				continue;
			double gap;
			if (dx > 0) {
				if (db->x < sb->x + sb->width) continue;
				gap = db->x - (sb->x + sb->width);
			} else {
				if (db->x + db->width > sb->x) continue;
				gap = sb->x - (db->x + db->width);
			}
			if (gap < best_gap) {
				best_gap = gap;
				best = m;
			}
		}

		if (best) {
			double ry = (cursor->y - sb->y) / (double)sb->height;
			if (ry < 0) ry = 0;
			if (ry > 1) ry = 1;
			double new_y = best->m.y + ry * best->m.height;
			if (new_y < best->m.y) new_y = best->m.y;
			if (new_y >= best->m.y + best->m.height)
				new_y = best->m.y + best->m.height - 1;

			double new_x = tx;
			if (new_x < best->m.x) new_x = best->m.x;
			if (new_x >= best->m.x + best->m.width)
				new_x = best->m.x + best->m.width - 1;

			wlr_cursor_warp(cursor, NULL, new_x, new_y);
			return 1;
		}
		/* No monitor in horizontal direction — fall through. */
	}

	/* CASE 2: vertical-dominant crossing. Look for a monitor directly
	 * above/below the current one whose horizontal range covers the
	 * cursor's current X. If found, remap X proportionally. If not,
	 * block motion at the current monitor's top/bottom edge. */
	if (crosses_v && !prefer_h) {
		Monitor *best = NULL;
		double best_gap = 1e30;
		Monitor *m;
		wl_list_for_each(m, &mons, link) {
			if (!m->wlr_output->enabled || m->is_mirror || m == cur_mon)
				continue;
			struct wlr_box *db = &m->m;
			if (db->width <= 0 || db->height <= 0)
				continue;
			double gap;
			if (dy > 0) {
				if (db->y < sb->y + sb->height) continue;
				gap = db->y - (sb->y + sb->height);
			} else {
				if (db->y + db->height > sb->y) continue;
				gap = sb->y - (db->y + db->height);
			}
			/* The neighbour must sit (at least partially) under or
			 * over the cursor's current X. */
			if (cursor->x < db->x || cursor->x >= db->x + db->width)
				continue;
			if (gap < best_gap) {
				best_gap = gap;
				best = m;
			}
		}

		if (!best) {
			/* No monitor in the vertical direction of motion — BLOCK
			 * the cursor at the current monitor's edge (mirror of the
			 * top-edge block for the bottom). */
			double cy = (dy > 0) ? sb->y + sb->height - 1 : sb->y;
			double cx = tx;
			if (cx < sb->x) cx = sb->x;
			if (cx >= sb->x + sb->width) cx = sb->x + sb->width - 1;
			wlr_cursor_warp(cursor, NULL, cx, cy);
			return 1;
		}

		/* Vertical neighbour exists — proportional X remapping. */
		double rx = (cursor->x - sb->x) / (double)sb->width;
		if (rx < 0) rx = 0;
		if (rx > 1) rx = 1;
		double new_x = best->m.x + rx * best->m.width;
		if (new_x < best->m.x) new_x = best->m.x;
		if (new_x >= best->m.x + best->m.width)
			new_x = best->m.x + best->m.width - 1;

		double new_y = ty;
		if (new_y < best->m.y) new_y = best->m.y;
		if (new_y >= best->m.y + best->m.height)
			new_y = best->m.y + best->m.height - 1;

		wlr_cursor_warp(cursor, NULL, new_x, new_y);
		return 1;
	}

	/* CASE 3: dead-zone fallback. Target doesn't land on any output (corner
	 * diagonal past a gap). Snap to the nearest valid edge of a neighbouring
	 * monitor in the dominant direction of motion. */
	if (wlr_output_layout_output_at(output_layout, tx, ty))
		return 0;

	int prefer_horiz = fabs(dx) >= fabs(dy);
	Monitor *m;
	Monitor *best = NULL;
	double best_dist = 1e30;
	double snap_x = tx, snap_y = ty;

	wl_list_for_each(m, &mons, link) {
		if (!m->wlr_output->enabled || m->is_mirror)
			continue;
		struct wlr_box *b = &m->m;
		if (b->width <= 0 || b->height <= 0)
			continue;

		if (prefer_horiz) {
			if (tx < b->x || tx >= b->x + b->width)
				continue;
			if (dx > 0 && b->x + b->width <= cursor->x)
				continue;
			if (dx < 0 && b->x >= cursor->x)
				continue;
			double cy = ty;
			if (cy < b->y) cy = b->y;
			if (cy >= b->y + b->height) cy = b->y + b->height - 1;
			double d = fabs(ty - cy);
			if (d < best_dist) {
				best_dist = d;
				snap_x = tx;
				snap_y = cy;
				best = m;
			}
		} else {
			if (ty < b->y || ty >= b->y + b->height)
				continue;
			if (dy > 0 && b->y + b->height <= cursor->y)
				continue;
			if (dy < 0 && b->y >= cursor->y)
				continue;
			double cx = tx;
			if (cx < b->x) cx = b->x;
			if (cx >= b->x + b->width) cx = b->x + b->width - 1;
			double d = fabs(tx - cx);
			if (d < best_dist) {
				best_dist = d;
				snap_x = cx;
				snap_y = ty;
				best = m;
			}
		}
	}

	if (!best)
		return 0;

	wlr_cursor_warp(cursor, NULL, snap_x, snap_y);
	return 1;
}

void
motionnotify(uint32_t time, struct wlr_input_device *device, double dx, double dy,
		double dx_unaccel, double dy_unaccel)
{
	int tiled = 0;
	double sx = 0, sy = 0, sx_confined, sy_confined;
	Client *c = NULL, *w = NULL;
	LayerSurface *l = NULL;
	struct wlr_surface *surface = NULL;
	/*
	 * Track input timestamp for mouse motion latency measurement.
	 * Mouse input is most critical for competitive gaming.
	 */
	if (game_mode_ultra && selmon && time > 0) {
		selmon->last_input_ns = get_time_ns();
	}

	/* time is 0 in internal calls meant to restore pointer focus. */
	if (time) {
		double raw_dx = dx, raw_dy = dy;

		/* DEBUG: one-shot trace of cursor/HW-plane state at first real motion */
		static int dbg_first_motion = 1;
		if (dbg_first_motion) {
			dbg_first_motion = 0;
			Monitor *dm = xytomon(cursor->x, cursor->y);
			wlr_log(WLR_INFO, "DBG first-motion: cursor=(%.1f,%.1f) mon=%s "
				"hw_cursor=%s ll_cursor=%s cpu_cursor=%s scale=%.2f",
				cursor->x, cursor->y,
				dm && dm->wlr_output ? dm->wlr_output->name : "(none)",
				dm && dm->wlr_output && dm->wlr_output->hardware_cursor ? "YES" : "NO",
				dm && dm->ll_cursor_active ? "YES" : "NO",
				cpu_cursor_active ? "YES" : "NO",
				dm && dm->wlr_output ? dm->wlr_output->scale : 0.0);
		}

		checkconstraint();

		if (active_constraint && cursor_mode != CurResize && cursor_mode != CurMove) {
			toplevel_from_wlr_surface(active_constraint->surface, &c, NULL);
			if (c && active_constraint->surface == seat->pointer_state.focused_surface) {
				/*
				 * Convert cursor (layout coords) → surface-local coords.
				 * scene_surface->node.x/y already accounts for c->bw
				 * AND the letterbox offset configurex11 applies when a
				 * fullscreen X11 game renders smaller than the monitor
				 * (common on ultrawide). Using c->bw alone breaks the
				 * conversion for letterboxed games — the cursor lands
				 * outside the constraint region and gets clamped to
				 * the rendered surface edge. Same fix needed across
				 * Intel/AMD/Nvidia — the bug only manifested on setups
				 * where the game's render size != monitor size.
				 */
				sx = cursor->x - c->geom.x - c->scene_surface->node.x;
				sy = cursor->y - c->geom.y - c->scene_surface->node.y;
				if (wlr_region_confine(&active_constraint->region, sx, sy,
						sx + dx, sy + dy, &sx_confined, &sy_confined)) {
					dx = sx_confined - sx;
					dy = sy_confined - sy;
				}

				if (active_constraint->type == WLR_POINTER_CONSTRAINT_V1_LOCKED) {
					wlr_relative_pointer_manager_v1_send_relative_motion(
							relative_pointer_mgr, seat,
							(uint64_t)time * 1000,
							raw_dx, raw_dy,
							dx_unaccel, dy_unaccel);
					return;
				}
			}
		}

		/* Smoothly cross dead zones between misaligned monitors so the
		 * cursor doesn't get stuck at the corner of a tall monitor when
		 * the neighbouring monitor is shorter (or vice versa). Skip when
		 * a pointer constraint or interactive move/resize is in effect,
		 * or when a fullscreen client is on the current monitor (existing
		 * fullscreen confinement below would yank it back anyway). */
		int snapped = 0;
		if (!active_constraint && cursor_mode == CurNormal) {
			int has_fs = 0;
			Client *fsc;
			wl_list_for_each(fsc, &clients, link) {
				if (fsc->isfullscreen && VISIBLEON(fsc, selmon)) {
					has_fs = 1;
					break;
				}
			}
			if (!has_fs)
				snapped = cursor_snap_across_dead_zone(dx, dy);
		}

		/* Move cursor as early as possible — the HW cursor plane updates
		 * via DRM ioctl here, so every µs saved before this call reduces
		 * visible cursor latency. */
		if (!snapped)
			wlr_cursor_move(cursor, device, dx, dy);
		wlr_idle_notifier_v1_notify_activity(idle_notifier, seat);

		/* Confine cursor to monitor when a fullscreen client is present.
		 * This prevents accidental mouse drift to other monitors during
		 * fullscreen games. Keybind-based warping (focusmon/warptomonitor)
		 * bypasses this because they use wlr_cursor_warp() directly and
		 * change selmon before subsequent motionnotify calls. */
		{
			Client *fsc = NULL;
			wl_list_for_each(fsc, &clients, link) {
				if (fsc->isfullscreen && VISIBLEON(fsc, selmon))
					break;
			}
			if (&fsc->link != &clients && fsc) {
				struct wlr_box *mb = &selmon->m;
				double cx = cursor->x, cy = cursor->y;
				int clamped = 0;
				if (cx < mb->x) { cx = mb->x; clamped = 1; }
				if (cy < mb->y) { cy = mb->y; clamped = 1; }
				if (cx >= mb->x + mb->width) { cx = mb->x + mb->width - 1; clamped = 1; }
				if (cy >= mb->y + mb->height) { cy = mb->y + mb->height - 1; clamped = 1; }
				if (clamped)
					wlr_cursor_warp(cursor, NULL, cx, cy);
			}
		}

		/*
		 * Force full damage on the monitor under the cursor when using
		 * software cursor (no HW cursor plane).
		 *
		 * When the HW cursor plane isn't available (or fails silently),
		 * wlroots falls back to software cursor — compositing the cursor
		 * image into the framebuffer.  With multi-buffer swapchains
		 * (double/triple buffering), each back buffer may still contain
		 * the cursor from a previous position.  If the damage ring
		 * doesn't cover those stale regions, old cursor images persist,
		 * creating a visible trail of ghost copies.
		 *
		 * Atomic commit failures (EBUSY) make this worse: the failed
		 * buffer is discarded, so the next buffer's age is off by one.
		 *
		 * When the HW cursor plane IS active, the cursor is on a
		 * separate DRM plane and never composited into the framebuffer,
		 * so no ghost trails or full damage are needed.  Forcing full
		 * damage with HW cursor causes tearing-flip-triggered black
		 * horizontal stripes in fullscreen games — each cursor motion
		 * event forces a composited frame committed with tearing page
		 * flip, and hundreds of those per second create multiple tear
		 * lines within a single display refresh.
		 */
		{
			Monitor *cm = xytomon(cursor->x, cursor->y);
			if (cm && cm->scene_output && !cm->wlr_output->hardware_cursor) {
				wlr_damage_ring_add_whole(&cm->scene_output->damage_ring);
				wlr_output_schedule_frame(cm->wlr_output);
			}
		}

		/* Relative pointer protocol — sent after the HW cursor plane
		 * update so the visible cursor position changes before any
		 * Wayland protocol I/O to clients. */
		wlr_relative_pointer_manager_v1_send_relative_motion(
				relative_pointer_mgr, seat, (uint64_t)time * 1000,
				raw_dx, raw_dy, dx_unaccel, dy_unaccel);

		/* Update selmon (even while dragging a window) */
		if (sloppyfocus) {
			Monitor *newmon = xytomon(cursor->x, cursor->y);
			if (newmon && newmon != selmon) {
				/* Transfer any open status menus to new monitor */
				transfer_status_menus(selmon, newmon);
				selmon = newmon;
				monitor_wake(newmon);
			}
		}
	}

	/* Screenshot dragging — update selection overlay and skip client dispatch */
	if (screenshot_mode == SCREENSHOT_DRAGGING) {
		screenshot_handle_motion();
		return;
	}

	/* Monitor setup popup drag tracking */
	{
		Monitor *ms_mon = monitor_setup_visible_monitor();
		if (ms_mon && ms_mon->monitor_setup.dragging >= 0) {
			monitor_setup_handle_motion(ms_mon,
				(int)lround(cursor->x), (int)lround(cursor->y));
			return;
		}
	}

	/* Find the client under the pointer at the NEW cursor position.
	 * Doing this after wlr_cursor_move() (instead of before) means the
	 * scene traversal doesn't block the HW cursor update. */
	xytonode(cursor->x, cursor->y, &surface, &c, NULL, &sx, &sy);

	/* Fullscreen game black-bar fallback.
	 * configurex11 offsets scene_surface to center games that render at
	 * a smaller resolution than the monitor.  xytonode returns surface=NULL
	 * when the cursor is in the resulting black bar area (fullscreen_bg is
	 * a rect, not a wlr_surface).  Without this fallback, pointerfocus()
	 * clears pointer focus and XWayland can't maintain pointer grab/lock,
	 * breaking mouse input for the game.
	 *
	 * Route all pointer events on the fullscreen monitor to the game's
	 * surface, clamping coordinates to the rendered area. */
	if (!surface && !c) {
		Client *fsc = NULL;
		wl_list_for_each(fsc, &clients, link) {
			if (fsc->isfullscreen && VISIBLEON(fsc, selmon))
				break;
		}
		if (&fsc->link != &clients && fsc && client_surface(fsc)) {
			c = fsc;
			surface = client_surface(fsc);
			/* scene_surface offset = game's visual position within c->scene */
			double surf_x = fsc->geom.x + fsc->scene_surface->node.x;
			double surf_y = fsc->geom.y + fsc->scene_surface->node.y;
			sx = cursor->x - surf_x;
			sy = cursor->y - surf_y;
			/* Clamp to rendered surface bounds */
			int sw = surface->current.width;
			int sh = surface->current.height;
			if (sx < 0) sx = 0;
			if (sy < 0) sy = 0;
			if (sx >= sw) sx = sw - 1;
			if (sy >= sh) sy = sh - 1;
		}
	}

	if (cursor_mode == CurPressed && !seat->drag
			&& surface != seat->pointer_state.focused_surface
			&& toplevel_from_wlr_surface(seat->pointer_state.focused_surface, &w, &l) >= 0) {
		c = w;
		surface = seat->pointer_state.focused_surface;
		sx = cursor->x - (l ? l->scene->node.x : w->geom.x);
		sy = cursor->y - (l ? l->scene->node.y : w->geom.y);
	}

	/* Update drag icon's position */
	wlr_scene_node_set_position(&drag_icon->node, (int)round(cursor->x), (int)round(cursor->y));

	/* Hover feedback for status bar modules.
	 * Skip when: bar hidden, fullscreen covers bar, or cursor is far from
	 * bar area (unless a popup is open that needs hover tracking).
	 * Throttled to ~125 Hz — hover highlights don't need 1 kHz updates
	 * and the saved cycles let motionnotify() return faster, keeping
	 * the event loop responsive for the next HW cursor plane update. */
	if (selmon && selmon->showbar && selmon->statusbar.area.height > 0) {
		Client *fs = focustop(selmon);
		int any_popup_open =
			selmon->statusbar.cpu_popup.visible ||
			selmon->statusbar.ram_popup.visible ||
			selmon->statusbar.battery_popup.visible ||
			selmon->statusbar.fan_popup.visible ||
			selmon->statusbar.net_menu.visible ||
			selmon->statusbar.tray_menu.visible;
		int near_bar = (cursor->y >= selmon->statusbar.area.y &&
			cursor->y <= selmon->statusbar.area.y +
				selmon->statusbar.area.height + 500);

		if (!(fs && fs->isfullscreen) && (near_bar || any_popup_open)) {
			/* Fan popup drag must respond at full input rate */
			if (selmon->statusbar.fan_popup.dragging)
				fan_popup_handle_drag(selmon, cursor->x, cursor->y);

			static uint32_t last_hover_ms;
			if (!time || time - last_hover_ms >= 8) {
				if (time) last_hover_ms = time;
				updatetaghover(selmon, cursor->x, cursor->y);
				updatecpuhover(selmon, cursor->x, cursor->y);
				updateramhover(selmon, cursor->x, cursor->y);
				updatebatteryhover(selmon, cursor->x, cursor->y);
				updatefanhover(selmon, cursor->x, cursor->y);
				updatenethover(selmon, cursor->x, cursor->y);
				net_menu_update_hover(selmon, cursor->x, cursor->y);
				if (!selmon->statusbar.net_menu.visible)
					net_menu_hide_all();
			}
		}
	}

	/* Skip if internal call or already resizing */
	if (time == 0 && resizing_from_mouse)
		goto focus;

	tiled = grabc && !grabc->isfloating && !grabc->isfullscreen;
	last_pointer_motion_ms = monotonic_msec();
	if (cursor_mode == CurMove) {
		/* Move the grabbed client to the new position. */
		if (grabc && grabc->isfloating) {
			resize(grabc, (struct wlr_box){
				.x = (int)round(cursor->x) - grabcx,
				.y = (int)round(cursor->y) - grabcy,
				.width = grabc->geom.width,
				.height = grabc->geom.height
			}, 1);
			return;
		}
	} else if (cursor_mode == CurResize) {
			if (!resize_should_update(time))
				goto focus;

			if (tiled && resizing_from_mouse) {
				double ratio, ratio_h;
				int changed = 0;
				double dx_total = cursor->x - resize_start_x;
				double dy_total = cursor->y - resize_start_y;
				LayoutNode *client_node;

				if (!resize_split_node && !resize_split_node_h) {
					LayoutNode **curr_root = get_current_root(selmon);
					client_node = curr_root ? find_client_node(*curr_root, grabc) : NULL;
					if (!client_node)
						goto focus;

					resize_use_v = resize_use_h = 0;

					resize_split_node = closest_split_node(client_node, 1, cursor->x,
							&resize_start_ratio_v, &resize_start_box_v, NULL);
					if (!resize_split_node)
						resize_start_ratio_v = 0.5;

					resize_split_node_h = closest_split_node(client_node, 0, cursor->y,
							&resize_start_ratio_h, &resize_start_box_h, NULL);
					if (!resize_split_node_h)
						resize_start_ratio_h = 0.5;

					apply_resize_axis_choice();
				}

				if (resize_use_v && resize_split_node && resize_start_box_v.width > 0) {
					double current = resize_split_node->split_ratio;
					ratio = resize_start_ratio_v + dx_total / resize_start_box_v.width;
						if (ratio < 0.05f)
							ratio = 0.05f;
						if (ratio > 0.95f)
							ratio = 0.95f;
						if (fabs(ratio - current) >= resize_ratio_epsilon) {
							float old_r = resize_split_node->split_ratio;
							resize_split_node->split_ratio = (float)ratio;
							compensate_column_resize(resize_split_node, old_r, (float)ratio, grabc);
							changed = 1;
						}
					}

				if (resize_use_h && resize_split_node_h && resize_start_box_h.height > 0) {
					double current_h = resize_split_node_h->split_ratio;
					ratio_h = resize_start_ratio_h + dy_total / resize_start_box_h.height;
						if (ratio_h < 0.05f)
							ratio_h = 0.05f;
						if (ratio_h > 0.95f)
							ratio_h = 0.95f;
						if (fabs(ratio_h - current_h) >= resize_ratio_epsilon) {
							resize_split_node_h->split_ratio = (float)ratio_h;
							changed = 1;
						}
					}

			if (changed)
				arrange(selmon);

		} else if (grabc && grabc->isfloating) {
			double dx_total = cursor->x - resize_start_x;
			double dy_total = cursor->y - resize_start_y;
			int minw = 1 + 2 * (int)grabc->bw;
			int minh = 1 + 2 * (int)grabc->bw;
			int dw = (int)lround(dx_total);
			int dh = (int)lround(dy_total);
			struct wlr_box box = resize_start_box_f;

			if (resize_dir_x) {
				if (resize_dir_x > 0) {
					if (box.width + dw < minw)
						dw = minw - box.width;
					box.width += dw;
				} else {
					if (box.width - dw < minw)
						dw = box.width - minw;
					box.x += dw;
					box.width -= dw;
				}
			}

			if (resize_dir_y) {
				if (resize_dir_y > 0) {
					if (box.height + dh < minh)
						dh = minh - box.height;
					box.height += dh;
				} else {
					if (box.height - dh < minh)
						dh = box.height - minh;
					box.y += dh;
					box.height -= dh;
				}
			}

			resize(grabc, box, 1);
			return;
		}
	}

focus:
	/* If there's no client surface under the cursor, set the cursor image to a
	 * default. This is what makes the cursor image appear when you move it
	 * off of a client or over its border. */
	if (!surface && !seat->drag)
		nixly_cursor_set_xcursor("default");

	pointerfocus(c, surface, sx, sy, time);
}

void
motionrelative(struct wl_listener *listener, void *data)
{
	/* This event is forwarded by the cursor when a pointer emits a _relative_
	 * pointer motion event (i.e. a delta) */
	struct wlr_pointer_motion_event *event = data;
	/* The cursor doesn't move unless we tell it to. The cursor automatically
	 * handles constraining the motion to the output layout, as well as any
	 * special configuration applied for the specific input device which
	 * generated the event. You can pass NULL for the device if you want to move
	 * the cursor around without any input. */
	last_pointer_motion_ms = monotonic_msec();
	motionnotify(event->time_msec, &event->pointer->base, event->delta_x, event->delta_y,
			event->unaccel_dx, event->unaccel_dy);
}

void
moveresize(const Arg *arg)
{
	if (cursor_mode != CurNormal && cursor_mode != CurPressed)
		return;
	xytonode(cursor->x, cursor->y, NULL, &grabc, NULL, NULL, NULL);
	if (!grabc || client_is_unmanaged(grabc) || grabc->isfullscreen)
		return;

	cursor_mode = arg->ui;
	grabc->was_tiled = (!grabc->isfloating && !grabc->isfullscreen);

	if (grabc->was_tiled) {
		switch (cursor_mode) {
		case CurMove:
			{
				struct wlr_box start_geom = grabc->geom;
				/* Start drag tracking before making floating */
				start_tile_drag(selmon, grabc);
				setfloating(grabc, 1);
				/* Anchor to the original cursor offset within the window */
				grabcx = (int)round(cursor->x) - start_geom.x;
				grabcy = (int)round(cursor->y) - start_geom.y;
				/* Keep the window anchored under the cursor when leaving tiling. */
				resize(grabc, start_geom, 1);
				nixly_cursor_set_xcursor("fleur");
				break;
			}
		case CurResize:
			{
				struct wlr_box start_geom = grabc->geom;
				const char *cursor_name = pick_resize_handle(grabc, cursor->x, cursor->y);
				double start_x = cursor->x, start_y = cursor->y;
				grabcx = (int)round(cursor->x);
				grabcy = (int)round(cursor->y);

				resize_start_box_v = (struct wlr_box){0};
				resize_start_box_h = (struct wlr_box){0};
				resize_start_box_f = start_geom;
				resize_start_x = start_x;
				resize_start_y = start_y;
				resize_start_ratio_v = resize_start_ratio_h = 0.0;
				resize_split_node = NULL;
				resize_split_node_h = NULL;
				resize_last_time = 0;
				resize_last_x = start_x;
				resize_last_y = start_y;
				resizing_from_mouse = 1;
				/* Keep geometry unchanged as we switch to floating resize. */
				resize(grabc, start_geom, 1);
				nixly_cursor_set_xcursor(cursor_name);
			}
			break;
		}
	} else {
		/* Default floating logic */
		/* Float the window and tell motionnotify to grab it */
		setfloating(grabc, 1);
		switch (cursor_mode) {
		case CurMove:
			grabcx = (int)round(cursor->x) - grabc->geom.x;
			grabcy = (int)round(cursor->y) - grabc->geom.y;
			nixly_cursor_set_xcursor("fleur");
			break;
		case CurResize:
			{
				const char *cursor_name = pick_resize_handle(grabc, cursor->x, cursor->y);
				double start_x = cursor->x, start_y = cursor->y;
				grabcx = (int)round(cursor->x);
				grabcy = (int)round(cursor->y);

				resize_start_box_v = (struct wlr_box){0};
				resize_start_box_h = (struct wlr_box){0};
				resize_start_box_f = grabc->geom;
				resize_start_x = start_x;
				resize_start_y = start_y;
				resize_start_ratio_v = resize_start_ratio_h = 0.0;
				resize_split_node = NULL;
				resize_split_node_h = NULL;
				resize_last_time = 0;
				resize_last_x = start_x;
				resize_last_y = start_y;
				resizing_from_mouse = 1;
				nixly_cursor_set_xcursor(cursor_name);
			}
			break;
		}
	}
}

void
pointerfocus(Client *c, struct wlr_surface *surface, double sx, double sy,
		uint32_t time)
{
	struct timespec now;
	static int in_pointerfocus = 0;

	/* Focus follows mouse: focus client under cursor.
	 * Also update focus on internal calls (time==0) after layout changes
	 * so the tile under the cursor gets focus after rebalancing.
	 * Use re-entry guard to prevent infinite recursion since focusclient
	 * can trigger motionnotify which calls pointerfocus again. */
	if (!in_pointerfocus && surface != seat->pointer_state.focused_surface &&
			sloppyfocus && c && !client_is_unmanaged(c)) {
		in_pointerfocus = 1;
		focusclient(c, 0);
		in_pointerfocus = 0;
	}

	/* If surface is NULL, clear pointer focus */
	if (!surface) {
		wlr_seat_pointer_notify_clear_focus(seat);
		return;
	}

	if (!time) {
		clock_gettime(CLOCK_MONOTONIC, &now);
		time = now.tv_sec * 1000 + now.tv_nsec / 1000000;
	}

	/* Log pointer focus changes to/from game windows */
	if (surface != seat->pointer_state.focused_surface && c &&
			(looks_like_game(c) || is_game_content(c)))
		game_log("CURSOR_FOCUS: appid='%s' title='%s' "
			"pos=%.0f,%.0f constraint=%s",
			client_get_appid(c) ? client_get_appid(c) : "(null)",
			client_get_title(c) ? client_get_title(c) : "(null)",
			cursor->x, cursor->y,
			active_constraint ? "active" : "none");

	/* Let the client know that the mouse cursor has entered one
	 * of its surfaces, and make keyboard focus follow if desired.
	 * wlroots makes this a no-op if surface is already focused */
	wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
	wlr_seat_pointer_notify_motion(seat, time, sx, sy);

	/* (Re-)activate pointer constraint if the newly focused surface has one.
	 * This ensures games get their pointer lock/confine as soon as focus
	 * returns, without waiting for a motion event. */
	checkconstraint();
}

void
requeststartdrag(struct wl_listener *listener, void *data)
{
	struct wlr_seat_request_start_drag_event *event = data;

	if (wlr_seat_validate_pointer_grab_serial(seat, event->origin,
			event->serial))
		wlr_seat_start_pointer_drag(seat, event->drag, event->serial);
	else
		wlr_data_source_destroy(event->drag->source);
}

void
setcursor(struct wl_listener *listener, void *data)
{
	/* This event is raised by the seat when a client provides a cursor image */
	struct wlr_seat_pointer_request_set_cursor_event *event = data;
	/* If we're "grabbing" the cursor, don't use the client's image, we will
	 * restore it after "grabbing" sending a leave event, followed by a enter
	 * event, which will result in the client requesting set the cursor surface */
	if (cursor_mode != CurNormal && cursor_mode != CurPressed)
		return;
	/* This can be sent by any client, so we check to make sure this one
	 * actually has pointer focus first. If so, we can tell the cursor to
	 * use the provided surface as the cursor image. It will set the
	 * hardware cursor on the output that it's currently on and continue to
	 * do so as the cursor moves between outputs. */
	if (event->seat_client == seat->pointer_state.focused_client)
		nixly_cursor_set_client_surface(event->surface,
				event->hotspot_x, event->hotspot_y);
}

void
setcursorshape(struct wl_listener *listener, void *data)
{
	struct wlr_cursor_shape_manager_v1_request_set_shape_event *event = data;
	if (cursor_mode != CurNormal && cursor_mode != CurPressed)
		return;
	/* This can be sent by any client, so we check to make sure this one
	 * actually has pointer focus first. If so, we can tell the cursor to
	 * use the provided cursor shape. */
	if (event->seat_client == seat->pointer_state.focused_client)
		nixly_cursor_set_xcursor(
				wlr_cursor_shape_v1_name(event->shape));
}

void
startdrag(struct wl_listener *listener, void *data)
{
	struct wlr_drag *drag = data;
	if (!drag->icon)
		return;

	drag->icon->data = &wlr_scene_drag_icon_create(drag_icon, drag->icon)->node;
	LISTEN_STATIC(&drag->icon->events.destroy, destroydragicon);
}

void
virtualkeyboard(struct wl_listener *listener, void *data)
{
	struct wlr_virtual_keyboard_v1 *kb = data;
	/* virtual keyboards shouldn't share keyboard group */
	KeyboardGroup *group = createkeyboardgroup();
	/* Set the keymap to match the group keymap */
	wlr_keyboard_set_keymap(&kb->keyboard, group->wlr_group->keyboard.keymap);
	LISTEN(&kb->keyboard.base.events.destroy, &group->destroy, destroykeyboardgroup);

	/* Add the new keyboard to the group */
	wlr_keyboard_group_add_keyboard(group->wlr_group, &kb->keyboard);

	/* IMPORTANT: Restore global keyboard group as seat keyboard.
	 * createkeyboardgroup() sets seat keyboard to the new group, but for virtual
	 * keyboards we must keep the global kb_group as seat keyboard to avoid
	 * use-after-free when the virtual keyboard is destroyed */
	wlr_seat_set_keyboard(seat, &kb_group->wlr_group->keyboard);
}

void
textinput_enable(struct wl_listener *listener, void *data)
{
	TextInput *ti_wrap = wl_container_of(listener, ti_wrap, enable);
	struct wlr_text_input_v3 *ti = ti_wrap->text_input;
	(void)data;

	wlr_log(WLR_INFO, "Text input enabled for surface %p", (void*)ti->focused_surface);
	active_text_input = ti;

}

void
textinput_disable(struct wl_listener *listener, void *data)
{
	TextInput *ti_wrap = wl_container_of(listener, ti_wrap, disable);
	struct wlr_text_input_v3 *ti = ti_wrap->text_input;
	(void)data;

	wlr_log(WLR_INFO, "Text input disabled");
	if (active_text_input == ti)
		active_text_input = NULL;
}

void
textinput_commit(struct wl_listener *listener, void *data)
{
	(void)listener;
	(void)data;
	/* Text input committed - nothing to do here for now */
}

void
textinput_destroy(struct wl_listener *listener, void *data)
{
	TextInput *ti_wrap = wl_container_of(listener, ti_wrap, destroy);
	struct wlr_text_input_v3 *ti = ti_wrap->text_input;
	(void)data;

	wlr_log(WLR_INFO, "Text input destroyed");
	if (active_text_input == ti)
		active_text_input = NULL;

	/* Remove listeners and free */
	wl_list_remove(&ti_wrap->enable.link);
	wl_list_remove(&ti_wrap->disable.link);
	wl_list_remove(&ti_wrap->commit.link);
	wl_list_remove(&ti_wrap->destroy.link);
	wl_list_remove(&ti_wrap->link);
	free(ti_wrap);
}

void
textinput(struct wl_listener *listener, void *data)
{
	struct wlr_text_input_v3 *ti = data;
	TextInput *ti_wrap;
	(void)listener;

	wlr_log(WLR_INFO, "New text input created");

	ti_wrap = ecalloc(1, sizeof(*ti_wrap));
	ti_wrap->text_input = ti;

	/* Set up listeners for this text input */
	LISTEN(&ti->events.enable, &ti_wrap->enable, textinput_enable);
	LISTEN(&ti->events.disable, &ti_wrap->disable, textinput_disable);
	LISTEN(&ti->events.commit, &ti_wrap->commit, textinput_commit);
	LISTEN(&ti->events.destroy, &ti_wrap->destroy, textinput_destroy);

	wl_list_insert(&text_inputs, &ti_wrap->link);

	/* If a surface already has keyboard focus, send enter to this text input
	 * but only if the text input belongs to the same client as the surface */
	struct wlr_surface *focused = seat->keyboard_state.focused_surface;
	if (focused) {
		struct wl_client *ti_client = wl_resource_get_client(ti->resource);
		struct wl_client *surface_client = wl_resource_get_client(focused->resource);
		if (ti_client == surface_client) {
			wlr_text_input_v3_send_enter(ti, focused);
		}
	}
}

void
text_input_focus_change(struct wlr_surface *old, struct wlr_surface *new)
{
	TextInput *ti_wrap;

	wl_list_for_each(ti_wrap, &text_inputs, link) {
		struct wlr_text_input_v3 *ti = ti_wrap->text_input;
		if (!ti || !ti->resource)
			continue;

		/* Send leave if text input has any focused surface */
		if (ti->focused_surface) {
			wlr_text_input_v3_send_leave(ti);
		}

		/* Send enter for new surface - only if text input belongs to same client */
		if (new && new->resource) {
			struct wl_client *ti_client = wl_resource_get_client(ti->resource);
			struct wl_client *surface_client = wl_resource_get_client(new->resource);
			if (ti_client == surface_client) {
				wlr_text_input_v3_send_enter(ti, new);
			}
		}
	}
}

void
virtualpointer(struct wl_listener *listener, void *data)
{
	struct wlr_virtual_pointer_v1_new_pointer_event *event = data;
	struct wlr_input_device *device = &event->new_pointer->pointer.base;

	wlr_cursor_attach_input_device(cursor, device);
	if (event->suggested_output)
		wlr_cursor_map_input_to_output(cursor, device, event->suggested_output);
}

