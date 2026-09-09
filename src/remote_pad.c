/* remote_pad.c — gamepad desktop navigation while streaming.
 *
 * Hold Select as a modifier so games never see the chords:
 *   Select + L2/R2   previous / next workspace
 *   Select + dpad    move between tiles
 *   Select + B       close tile
 *   Select + Start   launcher
 * Sticks always drive the pointer (see remote_mouse.c).
 *
 * Devices are opened non-exclusively; the pad is grabbed only while
 * Select is held, so the chord halves never reach a running game.
 */

#include <dirent.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "nixlytile.h"
#include "client.h"
#include "remote.h"

#define LONGS_FOR(bits) (((bits) + 8 * sizeof(long) - 1) / (8 * sizeof(long)))
#define TESTBIT(b, arr) (((arr)[(b) / (8 * sizeof(long))] >> ((b) % (8 * sizeof(long)))) & 1UL)

#define TRIGGER_ON 128 /* analog trigger threshold */

typedef struct {
	int fd;
	char path[64];
	struct wl_event_source *src;
	int select_down;
	int grabbed;
	int l2_down, r2_down;
	struct input_absinfo abs[ABS_RZ + 1];
	int has_abs[ABS_RZ + 1];
	struct wl_list link;
} PadDev;

static struct wl_list pads;
static int inotify_fd = -1;
static struct wl_event_source *inotify_src;
static int pad_inited;

static int
is_gamepad(int fd)
{
	unsigned long key_bits[LONGS_FOR(KEY_MAX + 1)];

	memset(key_bits, 0, sizeof(key_bits));
	if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits) < 0)
		return 0;
	return TESTBIT(BTN_GAMEPAD, key_bits)
		&& TESTBIT(BTN_TL, key_bits)
		&& TESTBIT(BTN_TR, key_bits);
}

static void
set_grab(PadDev *pad, int on)
{
	if (pad->grabbed == on)
		return;
	if (ioctl(pad->fd, EVIOCGRAB, on ? 1 : 0) == 0)
		pad->grabbed = on;
}

static void
remove_device(PadDev *pad)
{
	if (pad->grabbed)
		ioctl(pad->fd, EVIOCGRAB, 0);
	if (pad->src)
		wl_event_source_remove(pad->src);
	if (pad->fd >= 0)
		close(pad->fd);
	wl_list_remove(&pad->link);
	free(pad);
}

/* Map a raw axis onto [-1, 1] using the device's own range and flat zone */
static double
axis_norm(PadDev *pad, int code, int value)
{
	const struct input_absinfo *a;
	int mid, span;

	if (code > ABS_RZ || !pad->has_abs[code])
		return 0.0;
	a = &pad->abs[code];
	span = a->maximum - a->minimum;
	if (span <= 0)
		return 0.0;

	mid = a->minimum + span / 2;
	if (a->flat > 0 && value > mid - a->flat && value < mid + a->flat)
		return 0.0;

	return (double)(value - mid) / (double)(span / 2);
}

static void
action_i(void (*fn)(const Arg *), int i)
{
	Arg a = { .i = i };
	fn(&a);
}

static void
handle_chord(int code, int pressed)
{
	if (!pressed)
		return;

	switch (code) {
	case BTN_TL2:
		action_i(focus_workspace_dir, -1);
		break;
	case BTN_TR2:
		action_i(focus_workspace_dir, 1);
		break;
	case BTN_DPAD_LEFT:
		action_i(focus_column_dir, -1);
		break;
	case BTN_DPAD_RIGHT:
		action_i(focus_column_dir, 1);
		break;
	case BTN_DPAD_UP:
		action_i(focusstack, -1);
		break;
	case BTN_DPAD_DOWN:
		action_i(focusstack, 1);
		break;
	case BTN_EAST:
		killclient(&(Arg){0});
		break;
	case BTN_START: {
		Arg a = { .v = spawn_cmd_launcher };
		spawn(&a);
		break;
	}
	}
}

/* Analog triggers report as axes on most pads; fold them into the chord */
static void
handle_trigger(PadDev *pad, int code, int value)
{
	int on = value > TRIGGER_ON;
	int *state = code == ABS_Z ? &pad->l2_down : &pad->r2_down;

	if (*state == on)
		return;
	*state = on;
	if (on && pad->select_down)
		handle_chord(code == ABS_Z ? BTN_TL2 : BTN_TR2, 1);
}

/* Hat axis doubles as the dpad on many pads */
static void
handle_hat(PadDev *pad, int code, int value)
{
	if (!pad->select_down || value == 0)
		return;
	if (code == ABS_HAT0X)
		handle_chord(value < 0 ? BTN_DPAD_LEFT : BTN_DPAD_RIGHT, 1);
	else
		handle_chord(value < 0 ? BTN_DPAD_UP : BTN_DPAD_DOWN, 1);
}

static int
pad_event_cb(int fd, uint32_t mask, void *data)
{
	PadDev *pad = data;
	struct input_event ev;
	ssize_t n;

	if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
		remove_device(pad);
		return 0;
	}

	while ((n = read(fd, &ev, sizeof(ev))) == (ssize_t)sizeof(ev)) {
		if (!remote_is_active()) {
			if (pad->grabbed)
				set_grab(pad, 0);
			pad->select_down = 0;
			continue;
		}

		if (ev.type == EV_KEY) {
			if (ev.code == BTN_SELECT) {
				pad->select_down = (ev.value != 0);
				set_grab(pad, pad->select_down);
				continue;
			}
			if (ev.code == BTN_THUMBL) {
				remote_mouse_click(0, ev.value != 0);
				continue;
			}
			if (ev.code == BTN_THUMBR) {
				remote_mouse_click(1, ev.value != 0);
				continue;
			}
			if (pad->select_down)
				handle_chord(ev.code, ev.value != 0);
			continue;
		}

		if (ev.type != EV_ABS)
			continue;

		switch (ev.code) {
		case ABS_X: case ABS_Y: case ABS_RX: case ABS_RY:
			remote_mouse_axis(ev.code, axis_norm(pad, ev.code, ev.value));
			break;
		case ABS_Z: case ABS_RZ:
			handle_trigger(pad, ev.code, ev.value);
			break;
		case ABS_HAT0X: case ABS_HAT0Y:
			handle_hat(pad, ev.code, ev.value);
			break;
		}
	}

	if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK))
		remove_device(pad);
	return 0;
}

static void
try_add_device(const char *path)
{
	PadDev *pad;
	int fd, i;

	wl_list_for_each(pad, &pads, link) {
		if (strcmp(pad->path, path) == 0)
			return;
	}

	if ((fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC)) < 0)
		return;
	if (!is_gamepad(fd)) {
		close(fd);
		return;
	}
	if (!(pad = calloc(1, sizeof(*pad)))) {
		close(fd);
		return;
	}

	pad->fd = fd;
	snprintf(pad->path, sizeof(pad->path), "%s", path);
	for (i = 0; i <= ABS_RZ; i++) {
		if (ioctl(fd, EVIOCGABS(i), &pad->abs[i]) == 0)
			pad->has_abs[i] = 1;
	}

	pad->src = wl_event_loop_add_fd(event_loop, fd, WL_EVENT_READABLE,
					pad_event_cb, pad);
	if (!pad->src) {
		close(fd);
		free(pad);
		return;
	}
	wl_list_insert(&pads, &pad->link);
	wlr_log(WLR_INFO, "remote_pad: added %s", path);
}

static int
inotify_cb(int fd, uint32_t mask, void *data)
{
	char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
	ssize_t n;

	(void)mask;
	(void)data;

	while ((n = read(fd, buf, sizeof(buf))) > 0) {
		char *p = buf;
		while (p < buf + n) {
			struct inotify_event *iev = (struct inotify_event *)p;
			if (iev->len > 0 && strncmp(iev->name, "event", 5) == 0
					&& (iev->mask & (IN_CREATE | IN_ATTRIB))) {
				char path[64];
				snprintf(path, sizeof(path), "/dev/input/%s", iev->name);
				try_add_device(path);
			}
			p += sizeof(struct inotify_event) + iev->len;
		}
	}
	return 0;
}

static void
scan_devices(void)
{
	DIR *d = opendir("/dev/input");
	struct dirent *e;

	if (!d)
		return;
	while ((e = readdir(d))) {
		char path[288];
		if (strncmp(e->d_name, "event", 5) != 0)
			continue;
		snprintf(path, sizeof(path), "/dev/input/%s", e->d_name);
		try_add_device(path);
	}
	closedir(d);
}

void
remote_pad_init(void)
{
	if (pad_inited)
		return;
	wl_list_init(&pads);

	inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (inotify_fd >= 0) {
		inotify_add_watch(inotify_fd, "/dev/input",
				  IN_CREATE | IN_ATTRIB | IN_DELETE);
		inotify_src = wl_event_loop_add_fd(event_loop, inotify_fd,
						   WL_EVENT_READABLE,
						   inotify_cb, NULL);
	}
	scan_devices();
	pad_inited = 1;
}

void
remote_pad_cleanup(void)
{
	PadDev *pad, *tmp;

	remote_mouse_reset();
	wl_list_for_each_safe(pad, tmp, &pads, link)
		remove_device(pad);
	if (inotify_src) {
		wl_event_source_remove(inotify_src);
		inotify_src = NULL;
	}
	if (inotify_fd >= 0) {
		close(inotify_fd);
		inotify_fd = -1;
	}
	pad_inited = 0;
}
