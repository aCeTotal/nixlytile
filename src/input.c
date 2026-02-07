/* input.c - Auto-extracted from nixlytile.c */
#include "nixlytile.h"
#include "client.h"

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

	backlight_available = findbacklightdevice(backlight_brightness_path,
			sizeof(backlight_brightness_path),
			backlight_max_path, sizeof(backlight_max_path));

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

	steps = scrollsteps(event);
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
			StatusModule *tags = &selmon->statusbar.tags;
			StatusModule *mic = &selmon->statusbar.mic;
			StatusModule *vol = &selmon->statusbar.volume;
			StatusModule *cpu = &selmon->statusbar.cpu;
			StatusModule *sys = &selmon->statusbar.sysicons;
			StatusModule *bt = &selmon->statusbar.bluetooth;
			StatusModule *stm = &selmon->statusbar.steam;
			StatusModule *dsc = &selmon->statusbar.discord;

			if (selmon->statusbar.cpu_popup.visible) {
				if (cpu_popup_handle_click(selmon, lx, ly, button))
					return;
			}

			if (selmon->statusbar.ram_popup.visible) {
				if (ram_popup_handle_click(selmon, lx, ly, button))
					return;
			}

			if (lx >= 0 && ly >= 0 &&
					lx < selmon->statusbar.area.width &&
					ly < selmon->statusbar.area.height) {
				if (button == BTN_RIGHT &&
						sys->width > 0 &&
						lx >= sys->x && lx < sys->x + sys->width) {
					net_menu_hide_all();
					net_menu_open(selmon);
					return;
				}

				if (button == BTN_LEFT &&
						sys->width > 0 &&
						lx >= sys->x && lx < sys->x + sys->width) {
					Arg arg = { .v = netcmd };
					spawn(&arg);
					return;
				}

				if (cpu->width > 0 && lx >= cpu->x && lx < cpu->x + cpu->width) {
					Arg arg = { .v = btopcmd };
					spawn(&arg);
					return;
				}

				if (mic->width > 0 && lx >= mic->x && lx < mic->x + mic->width) {
					toggle_pipewire_mic_mute();
					return;
				}

				if (vol->width > 0 && lx >= vol->x && lx < vol->x + vol->width) {
					toggle_pipewire_mute();
					return;
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
					return;
				}

				/* Steam module click - focus or launch Steam */
				if (button == BTN_LEFT &&
						stm->width > 0 &&
						lx >= stm->x && lx < stm->x + stm->width) {
					focus_or_launch_app("steam", "steam");
					return;
				}

				/* Discord module click - focus or launch Discord */
				if (button == BTN_LEFT &&
						dsc->width > 0 &&
						lx >= dsc->x && lx < dsc->x + dsc->width) {
					focus_or_launch_app("discord", "discord");
					return;
				}

				if (button == BTN_LEFT && lx < tags->width) {
					for (int i = 0; i < tags->box_count; i++) {
						int bx = tags->box_x[i];
						int bw = tags->box_w[i];
						if (lx >= bx && lx < bx + bw) {
							Arg arg = { .ui = 1u << tags->box_tag[i] };
							view(&arg);
							return;
						}
					}
				}
			}
		}

		/* Change focus if the button was _pressed_ over a client */
		xytonode(cursor->x, cursor->y, NULL, &c, NULL, NULL, NULL);
		if (c && (!client_is_unmanaged(c) || client_wants_focus(c)))
			focusclient(c, 1);

		keyboard = wlr_seat_get_keyboard(seat);
		mods = keyboard ? wlr_keyboard_get_modifiers(keyboard) : 0;
		for (b = buttons; b < END(buttons); b++) {
			if (CLEANMASK(mods) == CLEANMASK(b->mod) &&
					button == b->button && b->func) {
				b->func(&b->arg);
				return;
			}
		}
	} else if (state == WL_POINTER_BUTTON_STATE_RELEASED) {
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
			StatusModule *tags = &selmon->statusbar.tags;
			StatusModule *mic = &selmon->statusbar.mic;
			StatusModule *vol = &selmon->statusbar.volume;
			StatusModule *cpu = &selmon->statusbar.cpu;
			StatusModule *sys = &selmon->statusbar.sysicons;
			StatusModule *bt = &selmon->statusbar.bluetooth;
			StatusModule *stm = &selmon->statusbar.steam;
			StatusModule *dsc = &selmon->statusbar.discord;

			if (selmon->statusbar.cpu_popup.visible) {
				if (cpu_popup_handle_click(selmon, lx, ly, event->button))
					return;
			}

			if (selmon->statusbar.ram_popup.visible) {
				if (ram_popup_handle_click(selmon, lx, ly, event->button))
					return;
			}

			if (lx >= 0 && ly >= 0 &&
					lx < selmon->statusbar.area.width &&
					ly < selmon->statusbar.area.height) {
				if (event->button == BTN_RIGHT &&
						sys->width > 0 &&
						lx >= sys->x && lx < sys->x + sys->width) {
					net_menu_hide_all();
					net_menu_open(selmon);
					return;
				}

				if (event->button == BTN_LEFT &&
						sys->width > 0 &&
						lx >= sys->x && lx < sys->x + sys->width) {
					Arg arg = { .v = netcmd };
					spawn(&arg);
					return;
				}

				if (cpu->width > 0 && lx >= cpu->x && lx < cpu->x + cpu->width) {
					Arg arg = { .v = btopcmd };
					spawn(&arg);
					return;
				}

				if (mic->width > 0 && lx >= mic->x && lx < mic->x + mic->width) {
					toggle_pipewire_mic_mute();
					return;
				}

				if (vol->width > 0 && lx >= vol->x && lx < vol->x + vol->width) {
					toggle_pipewire_mute();
					return;
				}

				/* Bluetooth module click - open bluetooth manager */
				if (event->button == BTN_LEFT &&
						bt->width > 0 &&
						lx >= bt->x && lx < bt->x + bt->width) {
					pid_t pid = fork();
					if (pid == 0) {
						setsid();
						execlp("blueman-manager", "blueman-manager", NULL);
						_exit(1);
					}
					return;
				}

				/* Steam module click - focus or launch Steam */
				if (event->button == BTN_LEFT &&
						stm->width > 0 &&
						lx >= stm->x && lx < stm->x + stm->width) {
					focus_or_launch_app("steam", "steam");
					return;
				}

				/* Discord module click - focus or launch Discord */
				if (event->button == BTN_LEFT &&
						dsc->width > 0 &&
						lx >= dsc->x && lx < dsc->x + dsc->width) {
					focus_or_launch_app("discord", "discord");
					return;
				}

				if (event->button == BTN_LEFT && lx < tags->width) {
					for (int i = 0; i < tags->box_count; i++) {
						int bx = tags->box_x[i];
						int bw = tags->box_w[i];
						if (lx >= bx && lx < bx + bw) {
							Arg arg = { .ui = 1u << tags->box_tag[i] };
							view(&arg);
							return;
						}
					}
				}
			}
		}

		/* Change focus if the button was _pressed_ over a client */
		xytonode(cursor->x, cursor->y, NULL, &c, NULL, NULL, NULL);
		if (c && (!client_is_unmanaged(c) || client_wants_focus(c)))
			focusclient(c, 1);

		keyboard = wlr_seat_get_keyboard(seat);
		mods = keyboard ? wlr_keyboard_get_modifiers(keyboard) : 0;
		for (b = buttons; b < END(buttons); b++) {
			if (CLEANMASK(mods) == CLEANMASK(b->mod) &&
					event->button == b->button && b->func) {
				b->func(&b->arg);
				return;
			}
		}
		break;
	case WL_POINTER_BUTTON_STATE_RELEASED:
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
			wlr_cursor_set_xcursor(cursor, cursor_mgr, "default");
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
	}

	wlr_cursor_attach_input_device(cursor, &pointer->base);
}

void
createpointerconstraint(struct wl_listener *listener, void *data)
{
	PointerConstraint *pointer_constraint = ecalloc(1, sizeof(*pointer_constraint));
	pointer_constraint->constraint = data;
	LISTEN(&pointer_constraint->constraint->events.destroy,
			&pointer_constraint->destroy, destroypointerconstraint);
}

void
cursorconstrain(struct wlr_pointer_constraint_v1 *constraint)
{
	if (active_constraint == constraint)
		return;

	if (active_constraint)
		wlr_pointer_constraint_v1_send_deactivated(active_constraint);

	active_constraint = constraint;
	wlr_pointer_constraint_v1_send_activated(constraint);
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
cursorwarptohint(void)
{
	Client *c = NULL;
	double sx = active_constraint->current.cursor_hint.x;
	double sy = active_constraint->current.cursor_hint.y;

	toplevel_from_wlr_surface(active_constraint->surface, &c, NULL);
	if (c && active_constraint->current.cursor_hint.enabled) {
		wlr_cursor_warp(cursor, NULL, sx + c->geom.x + c->bw, sy + c->geom.y + c->bw);
		wlr_seat_pointer_warp(active_constraint->seat, sx, sy);
	}
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
	default:
		/* TODO handle other input device types */
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
			k->func(&k->arg);
			return 1;
		}
	}
	return 0;
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
	xkb_keysym_t base_sym = xkb_state_key_get_one_sym(
			group->wlr_group->keyboard.xkb_state, keycode);
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
	Monitor *pc_gaming_mon = pc_gaming_visible_monitor();
	Monitor *stats_mon = stats_panel_visible_monitor();

	wlr_idle_notifier_v1_notify_activity(idle_notifier, seat);

	/* On _press_ if there is no active screen locker,
	 * attempt to process a compositor keybinding. */
	if (!locked && event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		/* Sudo password popup takes highest priority */
		if (sudo_mon) {
			for (i = 0; i < nsyms; i++) {
				if (sudo_popup_handle_key(sudo_mon, mods, syms[i]))
					handled = 1;
			}
		}
		/* WiFi password popup */
		if (wifi_mon && !handled) {
			for (i = 0; i < nsyms; i++) {
				if (wifi_popup_handle_key(wifi_mon, mods, syms[i]))
					handled = 1;
			}
		}
		/* Stats panel - handle before PC gaming so it can be used in-game */
		if (stats_mon && !handled) {
			for (i = 0; i < nsyms; i++) {
				if (stats_panel_handle_key(stats_mon, syms[i]))
					handled = 1;
			}
		}
		/* Video player - handle before other views */
		if (active_videoplayer && active_videoplayer->state != VP_STATE_IDLE && !handled) {
			for (i = 0; i < nsyms; i++) {
				if (videoplayer_handle_key(active_videoplayer, mods, syms[i])) {
					handled = 1;
					/* Check if player was stopped (Escape/Q) */
					if (active_videoplayer->state == VP_STATE_IDLE) {
						videoplayer_set_visible(active_videoplayer, 0);
						playback_state = PLAYBACK_IDLE;
					}
				}
			}
		}
		/* PC Gaming view */
		if (pc_gaming_mon && !handled) {
			for (i = 0; i < nsyms; i++) {
				if (pc_gaming_handle_key(pc_gaming_mon, mods, syms[i]))
					handled = 1;
			}
		}
		/* Media views (Movies/TV-shows) */
		if (!handled) {
			Monitor *media_mon = media_view_visible_monitor();
			if (media_mon) {
				for (i = 0; i < nsyms; i++) {
					/* Check which view is active */
					if (htpc_view_is_active(media_mon, media_mon->movies_view.view_tag, media_mon->movies_view.visible)) {
						if (media_view_handle_key(media_mon, MEDIA_VIEW_MOVIES, syms[i]))
							handled = 1;
					} else if (htpc_view_is_active(media_mon, media_mon->tvshows_view.view_tag, media_mon->tvshows_view.visible)) {
						if (media_view_handle_key(media_mon, MEDIA_VIEW_TVSHOWS, syms[i]))
							handled = 1;
					}
				}
			}
		}
		/* Nixpkgs popup */
		if (nixpkgs_mon && !handled) {
			int consumed = 0;
			int is_navigation = 0;
			for (i = 0; i < nsyms; i++) {
				xkb_keysym_t s = syms[i];
				if (nixpkgs_handle_key(nixpkgs_mon, mods, s)) {
					handled = 1;
					consumed = 1;
				}
				if (s == XKB_KEY_Up || s == XKB_KEY_Down)
					is_navigation = 1;
			}
			if (consumed) {
				if (is_navigation) {
					nixpkgs_update_selection(nixpkgs_mon);
				} else {
					nixpkgs_update_results(nixpkgs_mon);
					nixpkgs_render(nixpkgs_mon);
				}
			}
		}
		if (modal_mon) {
			int consumed = 0;
			int is_text_input = 0;
			for (i = 0; i < nsyms; i++) {
				xkb_keysym_t s = syms[i];
				/* Check if this is a text input key (printable or backspace) */
				if ((s >= 0x20 && s <= 0x7e) || s == XKB_KEY_BackSpace)
					is_text_input = 1;
				if (modal_handle_key(modal_mon, mods, s)) {
					handled = 1;
					consumed = 1;
				}
			}
			if (consumed) {
				int is_file_search = (modal_mon->modal.active_idx == 1);
				int is_navigation = 0;
				for (i = 0; i < nsyms; i++) {
					if (syms[i] == XKB_KEY_Up || syms[i] == XKB_KEY_Down) {
						is_navigation = 1;
						break;
					}
				}
				if (is_navigation) {
					/* Navigation keys: ultra-fast highlight toggle only */
					modal_update_selection(modal_mon);
				} else if (is_text_input && is_file_search) {
					/* File search: show text immediately, delay search 300ms */
					modal_render_search_field(modal_mon);
					/* Schedule search + full render after 300ms */
					if (!modal_mon->modal.render_timer)
						modal_mon->modal.render_timer = wl_event_loop_add_timer(
							event_loop, modal_render_timer_cb, modal_mon);
					modal_mon->modal.render_pending = 1;
					wl_event_source_timer_update(modal_mon->modal.render_timer, 300);
				} else {
					/* App launcher, git-projects, text input: full update */
					modal_update_results(modal_mon);
					modal_render(modal_mon);
				}
			}
		}
		if (!handled) {
			for (i = 0; i < nsyms; i++)
				handled = keybinding(mods, syms[i]) || handled;
		}
		/* If no binding matched, try with base keysyms (level 0) for shifted bindings */
		if (!handled && nlevel0 > 0) {
			for (i = 0; i < nlevel0; i++)
				handled = keybinding(mods, level0_syms[i]) || handled;
		}
	}

	/* Don't enable key repeat for modal/nixpkgs text input to avoid double characters.
	 * Only allow repeat for navigation keys (arrows, backspace) in modal/nixpkgs. */
	{
		int allow_repeat = 0;
		if (handled && group->wlr_group->keyboard.repeat_info.delay > 0) {
			if (!modal_mon && !nixpkgs_mon) {
				allow_repeat = 1;
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

__attribute__((unused)) LayoutNode *
ancestor_split(LayoutNode *node, int want_vert)
{
	if (!node)
		return NULL;
	node = node->split_node;
	while (node) {
		if (!node->is_client_node && node->is_split_vertically == (unsigned int)want_vert)
			return node;
		node = node->split_node;
	}
	return NULL;
}

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

void
motionnotify(uint32_t time, struct wlr_input_device *device, double dx, double dy,
		double dx_unaccel, double dy_unaccel)
{
	int tiled = 0;
	double sx = 0, sy = 0, sx_confined, sy_confined;
	Client *c = NULL, *w = NULL;
	LayerSurface *l = NULL;
	struct wlr_surface *surface = NULL;
	struct wlr_pointer_constraint_v1 *constraint;

	/*
	 * Track input timestamp for mouse motion latency measurement.
	 * Mouse input is most critical for competitive gaming.
	 */
	if (game_mode_ultra && selmon && time > 0) {
		selmon->last_input_ns = get_time_ns();
	}

	/* Find the client under the pointer and send the event along. */
	xytonode(cursor->x, cursor->y, &surface, &c, NULL, &sx, &sy);

	if (cursor_mode == CurPressed && !seat->drag
			&& surface != seat->pointer_state.focused_surface
			&& toplevel_from_wlr_surface(seat->pointer_state.focused_surface, &w, &l) >= 0) {
		c = w;
		surface = seat->pointer_state.focused_surface;
		sx = cursor->x - (l ? l->scene->node.x : w->geom.x);
		sy = cursor->y - (l ? l->scene->node.y : w->geom.y);
	}

	/* time is 0 in internal calls meant to restore pointer focus. */
	if (time) {
		wlr_relative_pointer_manager_v1_send_relative_motion(
				relative_pointer_mgr, seat, (uint64_t)time * 1000,
				dx, dy, dx_unaccel, dy_unaccel);

		wl_list_for_each(constraint, &pointer_constraints->constraints, link)
			cursorconstrain(constraint);

		if (active_constraint && cursor_mode != CurResize && cursor_mode != CurMove) {
			toplevel_from_wlr_surface(active_constraint->surface, &c, NULL);
			if (c && active_constraint->surface == seat->pointer_state.focused_surface) {
				sx = cursor->x - c->geom.x - c->bw;
				sy = cursor->y - c->geom.y - c->bw;
				if (wlr_region_confine(&active_constraint->region, sx, sy,
						sx + dx, sy + dy, &sx_confined, &sy_confined)) {
					dx = sx_confined - sx;
					dy = sy_confined - sy;
				}

				if (active_constraint->type == WLR_POINTER_CONSTRAINT_V1_LOCKED)
					return;
			}
		}

		wlr_cursor_move(cursor, device, dx, dy);
		wlr_idle_notifier_v1_notify_activity(idle_notifier, seat);

		/* Update selmon (even while dragging a window) */
		if (sloppyfocus) {
			Monitor *newmon = xytomon(cursor->x, cursor->y);
			if (newmon && newmon != selmon) {
				/* Transfer any open status menus to new monitor */
				transfer_status_menus(selmon, newmon);
				selmon = newmon;
			}
		}
	}

	/* Update drag icon's position */
	wlr_scene_node_set_position(&drag_icon->node, (int)round(cursor->x), (int)round(cursor->y));

	/* Hover feedback for tag boxes */
		if (selmon && selmon->showbar) {
			updatetaghover(selmon, cursor->x, cursor->y);
			updatecpuhover(selmon, cursor->x, cursor->y);
			updateramhover(selmon, cursor->x, cursor->y);
			updatebatteryhover(selmon, cursor->x, cursor->y);
			updatenethover(selmon, cursor->x, cursor->y);
			net_menu_update_hover(selmon, cursor->x, cursor->y);
			if (!selmon->statusbar.net_menu.visible)
				net_menu_hide_all();
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
							resize_split_node->split_ratio = (float)ratio;
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
	if (!surface && !seat->drag) {
		/* Hide cursor when HTPC media views are visible */
		if (htpc_mode_active && selmon &&
		    (selmon->movies_view.visible || selmon->tvshows_view.visible ||
		     selmon->pc_gaming.visible || selmon->retro_gaming.visible))
			wlr_cursor_set_surface(cursor, NULL, 0, 0);
		else
			wlr_cursor_set_xcursor(cursor, cursor_mgr, "default");
	}

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
				wlr_cursor_set_xcursor(cursor, cursor_mgr, "fleur");
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
				wlr_cursor_set_xcursor(cursor, cursor_mgr, cursor_name);
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
			wlr_cursor_set_xcursor(cursor, cursor_mgr, "fleur");
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
				wlr_cursor_set_xcursor(cursor, cursor_mgr, cursor_name);
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

	/* Let the client know that the mouse cursor has entered one
	 * of its surfaces, and make keyboard focus follow if desired.
	 * wlroots makes this a no-op if surface is already focused */
	wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
	wlr_seat_pointer_notify_motion(seat, time, sx, sy);
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
	/* Keep cursor hidden in HTPC views */
	if (selmon && (selmon->pc_gaming.visible ||
	               selmon->movies_view.visible ||
	               selmon->tvshows_view.visible))
		return;
	/* This can be sent by any client, so we check to make sure this one
	 * actually has pointer focus first. If so, we can tell the cursor to
	 * use the provided surface as the cursor image. It will set the
	 * hardware cursor on the output that it's currently on and continue to
	 * do so as the cursor moves between outputs. */
	if (event->seat_client == seat->pointer_state.focused_client)
		wlr_cursor_set_surface(cursor, event->surface,
				event->hotspot_x, event->hotspot_y);
}

void
setcursorshape(struct wl_listener *listener, void *data)
{
	struct wlr_cursor_shape_manager_v1_request_set_shape_event *event = data;
	if (cursor_mode != CurNormal && cursor_mode != CurPressed)
		return;
	/* Keep cursor hidden in HTPC views */
	if (selmon && (selmon->pc_gaming.visible ||
	               selmon->movies_view.visible ||
	               selmon->tvshows_view.visible))
		return;
	/* This can be sent by any client, so we check to make sure this one
	 * actually has pointer focus first. If so, we can tell the cursor to
	 * use the provided cursor shape. */
	if (event->seat_client == seat->pointer_state.focused_client)
		wlr_cursor_set_xcursor(cursor, cursor_mgr,
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

	/* Show OSK in HTPC mode when text input is enabled and controller is connected */
	if (htpc_mode_active && selmon && !wl_list_empty(&gamepads)) {
		osk_show(selmon, ti->focused_surface);
	}
}

void
textinput_disable(struct wl_listener *listener, void *data)
{
	TextInput *ti_wrap = wl_container_of(listener, ti_wrap, disable);
	struct wlr_text_input_v3 *ti = ti_wrap->text_input;
	(void)data;

	wlr_log(WLR_INFO, "Text input disabled");
	if (active_text_input == ti) {
		active_text_input = NULL;
		/* Hide OSK when text input is disabled */
		if (htpc_mode_active) {
			osk_hide_all();
		}
	}
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
	if (active_text_input == ti) {
		active_text_input = NULL;
		if (htpc_mode_active) {
			osk_hide_all();
		}
	}

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

