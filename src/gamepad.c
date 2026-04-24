#include "nixlytile.h"
#include "client.h"

void
gamepad_menu_show(Monitor *m)
{
	(void)m;
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
	(void)m;
}

int
gamepad_menu_handle_click(Monitor *m, int cx, int cy, uint32_t button)
{
	(void)cx;
	(void)cy;
	(void)button;

	if (!m)
		return 0;

	if (!m->gamepad_menu.visible)
		return 0;

	gamepad_menu_hide(m);
	return 1;
}

void
gamepad_menu_select(Monitor *m)
{
	(void)m;
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

	/* Mouse click emulation with shoulder buttons (when menu is not visible) */
	if (!gm->visible) {
		/* Skip click emulation if a fullscreen client has focus (let game handle it) */
		Client *focused = focustop(selmon);
		if (focused && focused->isfullscreen)
			return 0;

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

/* Helper to check if a bit is set in a bitfield */
#define BITFIELD_TEST(bits, bit) \
	((bits)[(bit) / (8 * sizeof(unsigned long))] & (1UL << ((bit) % (8 * sizeof(unsigned long)))))

int
gamepad_is_gamepad_device(int fd)
{
	unsigned long evbit[((EV_MAX) / (8 * sizeof(unsigned long))) + 1] = {0};
	unsigned long keybit[((KEY_MAX) / (8 * sizeof(unsigned long))) + 1] = {0};
	unsigned long absbit[((ABS_MAX) / (8 * sizeof(unsigned long))) + 1] = {0};
	char name[128] = "?";

	ioctl(fd, EVIOCGNAME(sizeof(name)), name);

	/* Get event types */
	if (ioctl(fd, EVIOCGBIT(0, sizeof(evbit)), evbit) < 0)
		return 0;

	/* Must have EV_KEY */
	if (!(evbit[0] & (1 << EV_KEY)))
		return 0;

	/* Get key capabilities */
	if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybit)), keybit) < 0)
		return 0;

	/* Reject if it has typical keyboard keys (KEY_A through KEY_Z) */
	if (BITFIELD_TEST(keybit, KEY_A))
		return 0;

	/* Reject if it has BTN_LEFT (mouse) */
	if (BITFIELD_TEST(keybit, BTN_LEFT))
		return 0;

	/* Check for any gamepad-specific buttons:
	 * BTN_SOUTH/BTN_A (0x130), BTN_EAST/BTN_B (0x131),
	 * BTN_NORTH/BTN_X (0x133), BTN_WEST/BTN_Y (0x134),
	 * BTN_TL (0x136), BTN_TR (0x137),
	 * BTN_SELECT (0x13a), BTN_START (0x13b),
	 * BTN_MODE (0x13c), BTN_THUMBL (0x13d), BTN_THUMBR (0x13e) */
	int has_gamepad_btn = 0;
	static const int gamepad_btns[] = {
		BTN_SOUTH, BTN_EAST, BTN_NORTH, BTN_WEST,
		BTN_TL, BTN_TR, BTN_SELECT, BTN_START,
		BTN_MODE, BTN_THUMBL, BTN_THUMBR,
	};
	for (size_t i = 0; i < sizeof(gamepad_btns) / sizeof(gamepad_btns[0]); i++) {
		if (BITFIELD_TEST(keybit, gamepad_btns[i])) {
			has_gamepad_btn = 1;
			break;
		}
	}

	/* Also accept devices with BTN_TRIGGER (joystick) + analog sticks */
	if (!has_gamepad_btn && BITFIELD_TEST(keybit, BTN_TRIGGER)) {
		if (evbit[0] & (1 << EV_ABS)) {
			ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absbit)), absbit);
			if (BITFIELD_TEST(absbit, ABS_X) && BITFIELD_TEST(absbit, ABS_Y))
				has_gamepad_btn = 1;
		}
	}

	if (!has_gamepad_btn) {
		wlr_log(WLR_DEBUG, "Not a gamepad (no gamepad buttons): %s", name);
	}

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

			/* Re-arm cursor timer on stick axis input (it stops when sticks are at rest) */
			if ((ev.code == ABS_X || ev.code == ABS_Y ||
			     ev.code == ABS_RX || ev.code == ABS_RY) &&
			    gamepad_cursor_timer)
				wl_event_source_timer_update(gamepad_cursor_timer, GAMEPAD_CURSOR_INTERVAL_MS);

			switch (ev.code) {
			case ABS_HAT0X:
				/* D-pad left/right for navigation */
				if (ev.value != 0) {
					int button = ev.value < 0 ? BTN_DPAD_LEFT : BTN_DPAD_RIGHT;

					/* Check OSK first - highest priority */
					Monitor *osk_mon = osk_visible_monitor();
					if (osk_mon) {
						osk_handle_button(osk_mon, button, 1);
						/* Start repeat timer for hold-to-slide */
						osk_dpad_repeat_start(osk_mon, button);
					}
				} else {
					/* D-pad released - stop OSK repeat if active */
					if (osk_dpad_held_button == BTN_DPAD_LEFT || osk_dpad_held_button == BTN_DPAD_RIGHT)
						osk_dpad_repeat_stop();
				}
				break;
			case ABS_HAT0Y:
				/* D-pad up/down for navigation */
				if (ev.value != 0) {
					int button = ev.value < 0 ? BTN_DPAD_UP : BTN_DPAD_DOWN;

					/* Check OSK first - highest priority */
					Monitor *osk_mon_y = osk_visible_monitor();
					if (osk_mon_y) {
						osk_handle_button(osk_mon_y, button, 1);
						/* Start repeat timer for hold-to-slide */
						osk_dpad_repeat_start(osk_mon_y, button);
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
	if (fd < 0) {
		wlr_log(WLR_INFO, "Gamepad: cannot open %s: %s", path, strerror(errno));
		return;
	}

	/* Get device name early for logging */
	ioctl(fd, EVIOCGNAME(sizeof(name)), name);

	/* Check if it's a gamepad */
	if (!gamepad_is_gamepad_device(fd)) {
		close(fd);
		return;
	}

	wlr_log(WLR_INFO, "Gamepad: detected %s at %s", name, path);

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

	/* Detect bus type (USB vs Bluetooth) */
	{
		struct input_id id;
		gp->is_bluetooth = 0;
		if (ioctl(fd, EVIOCGID, &id) == 0 && id.bustype == BUS_BLUETOOTH)
			gp->is_bluetooth = 1;
		wlr_log(WLR_INFO, "Gamepad transport: %s", gp->is_bluetooth ? "bluetooth" : "usb");
	}

	/* Initialize activity tracking */
	gp->last_activity_ms = monotonic_msec();
	gp->suspended = 0;
	gp->grabbed = 0;
	/* Record connect time - ignore button presses within 500ms to filter
	 * synthetic events Bluetooth controllers send on reconnect */
	gp->connect_time_ms = monotonic_msec();

	/* Try exclusive grab - if another process (e.g. libinput) already has the
	 * device grabbed, we can read the fd but won't receive events.  Re-open
	 * with write access for EVIOCGRAB compatibility. */
	{
		int rwfd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
		if (rwfd >= 0) {
			close(fd);
			fd = rwfd;
			gp->fd = fd;
		}
	}

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

	/* Switch TV/Monitor to this HDMI input via CEC */
	cec_switch_to_active_source();
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

static int gamepad_rescan_pending = 0;

int
gamepad_pending_timer_cb(void *data)
{
	int i;

	(void)data;

	if (gamepad_pending_count > 0) {
		int added_before = 0;
		GamepadDevice *gp;
		wl_list_for_each(gp, &gamepads, link)
			added_before++;

		for (i = 0; i < gamepad_pending_count; i++) {
			gamepad_device_add(gamepad_pending_paths[i]);
		}
		gamepad_pending_count = 0;

		/* If no new gamepad was found, schedule a full rescan in 1s
		 * to catch USB controllers that initialize slowly */
		int added_after = 0;
		wl_list_for_each(gp, &gamepads, link)
			added_after++;

		if (added_after == added_before) {
			gamepad_rescan_pending = 1;
			wl_event_source_timer_update(gamepad_pending_timer, 1000);
		}
	} else if (gamepad_rescan_pending) {
		gamepad_rescan_pending = 0;
		wlr_log(WLR_INFO, "Gamepad: late rescan for slow-init USB controllers");
		gamepad_scan_devices();
	}

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

	/* Schedule timer to add pending devices after delay.
	 * USB controllers (especially Xbox via xpad/hid) may need extra time
	 * for the kernel driver to fully initialize capabilities. */
	if (schedule_pending && gamepad_pending_timer) {
		wl_event_source_timer_update(gamepad_pending_timer, 300); /* 300ms */
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

void
gamepad_update_cursor(void)
{
	GamepadDevice *gp;
	double total_dx = 0, total_dy = 0;
	int any_input = 0;
	uint64_t now = monotonic_msec();

	if (wl_list_empty(&gamepads))
		return;

	/* Check if we should skip cursor movement for fullscreen clients
	 * (let game handle joystick input), but always allow right stick scrolling */
	int skip_cursor_move = 0;
	Client *focused = focustop(selmon);
	if (focused && focused->isfullscreen) {
		if (is_browser_client(focused)) {
			/* Browser fullscreen - still allow scroll */
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

static int
gamepad_any_stick_active(void)
{
	GamepadDevice *gp;

	wl_list_for_each(gp, &gamepads, link) {
		if (gp->suspended)
			continue;

		int lx_offset = gp->left_x - gp->cal_lx.center;
		int ly_offset = gp->left_y - gp->cal_ly.center;
		int lx_range = (gp->cal_lx.max - gp->cal_lx.min) / 2;
		int ly_range = (gp->cal_ly.max - gp->cal_ly.min) / 2;
		if (lx_range == 0) lx_range = 32767;
		if (ly_range == 0) ly_range = 32767;
		int lx_dz = gp->cal_lx.flat > 0 ? gp->cal_lx.flat : (lx_range * GAMEPAD_DEADZONE / 32767);
		int ly_dz = gp->cal_ly.flat > 0 ? gp->cal_ly.flat : (ly_range * GAMEPAD_DEADZONE / 32767);
		if (abs(lx_offset) > lx_dz || abs(ly_offset) > ly_dz)
			return 1;

		int rx_offset = gp->right_x - gp->cal_rx.center;
		int ry_offset = gp->right_y - gp->cal_ry.center;
		int rx_range = (gp->cal_rx.max - gp->cal_rx.min) / 2;
		int ry_range = (gp->cal_ry.max - gp->cal_ry.min) / 2;
		if (rx_range == 0) rx_range = 32767;
		if (ry_range == 0) ry_range = 32767;
		int rx_dz = gp->cal_rx.flat > 0 ? gp->cal_rx.flat : (rx_range * GAMEPAD_DEADZONE / 32767);
		int ry_dz = gp->cal_ry.flat > 0 ? gp->cal_ry.flat : (ry_range * GAMEPAD_DEADZONE / 32767);
		if (abs(rx_offset) > rx_dz || abs(ry_offset) > ry_dz)
			return 1;
	}
	return 0;
}

int
gamepad_cursor_timer_cb(void *data)
{
	(void)data;

	/* Update cursor position */
	gamepad_update_cursor();

	/* Only reschedule if a stick is deflected beyond deadzone */
	if (!wl_list_empty(&gamepads) && gamepad_cursor_timer && gamepad_any_stick_active()) {
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

		/* USB gamepads: never auto-suspend — keep alive regardless of activity */
		if (!gp->is_bluetooth)
			continue;

		/* Bluetooth gamepads: disconnect after 4 min inactivity or monitors off */
		if (!monitors_active ||
		    (now - gp->last_activity_ms) >= GAMEPAD_BT_INACTIVITY_TIMEOUT_MS) {
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

