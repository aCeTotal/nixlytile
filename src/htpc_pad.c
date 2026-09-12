/*
 * htpc_pad.c — HTPC gamepad workspace navigation.
 *
 * Shoulder buttons pass through to the running app untouched (devices
 * are opened non-exclusively, never grabbed).  Holding L1 or R1 for
 * 1.5 s slides one workspace back/forward with the normal vertical
 * slide animation; keeping it held keeps sliding every 600 ms until
 * the last populated workspace in that direction.
 *
 * Only active in htpc mode (htpc_mode_active).  Same non-exclusive
 * evdev scan + inotify hotplug pattern as apptoggle.c.
 */
#include "nixlytile.h"
#include "client.h"

#include <dirent.h>
#include <fcntl.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define HOLD_MS   1500
#define REPEAT_MS 600
#define LONGS_FOR(bits) (((bits) + 8 * sizeof(long) - 1) / (8 * sizeof(long)))
#define TESTBIT(b, arr) (((arr)[(b) / (8 * sizeof(long))] >> ((b) % (8 * sizeof(long)))) & 1UL)

typedef struct {
	int fd;
	char path[64];
	struct wl_event_source *src;
	int l1_down;
	int r1_down;
	struct wl_list link;
} HtpcPad;

static struct wl_list pads_list;
static int inotify_fd = -1;
static struct wl_event_source *inotify_src;
static struct wl_event_source *hold_timer;
static int hold_dir;   /* -1 = L1 held, +1 = R1 held, 0 = none */
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
remove_device(HtpcPad *gp)
{
	if (gp->src)
		wl_event_source_remove(gp->src);
	if (gp->fd >= 0)
		close(gp->fd);
	wl_list_remove(&gp->link);
	free(gp);
}

static int
step_workspace(void)
{
	int cur, max_idx, target;
	Arg a;

	if (!hold_dir || !selmon || !selmon->active_ws)
		return 0;
	cur = selmon->active_ws->idx;
	max_idx = workspace_max_nonempty_idx(selmon);
	if (max_idx < 0)
		return 0;
	target = cur + hold_dir;
	if (target < 0)
		target = 0;
	if (target > max_idx)
		target = max_idx;
	if (target == cur)
		return 0;
	a.i = target;
	focus_workspace_n(&a);
	return 1;
}

static int
hold_timer_cb(void *data)
{
	(void)data;
	if (!hold_dir)
		return 0;
	/* Keep repeating while held; once clamped at the end this just
	 * re-arms without switching, so releasing needs no bookkeeping. */
	step_workspace();
	if (hold_timer)
		wl_event_source_timer_update(hold_timer, REPEAT_MS);
	return 0;
}

/* Aggregate shoulder state over every connected pad.  Exactly one side
 * held → arm the 1.5 s hold; both or neither → cancel. */
static void
reevaluate(void)
{
	HtpcPad *gp;
	int l = 0, r = 0, dir;

	wl_list_for_each(gp, &pads_list, link) {
		l |= gp->l1_down;
		r |= gp->r1_down;
	}
	dir = (l && !r) ? -1 : (r && !l) ? 1 : 0;
	if (dir == hold_dir)
		return;
	hold_dir = dir;
	if (hold_timer)
		wl_event_source_timer_update(hold_timer, dir ? HOLD_MS : 0);
}

/* Exclusive grab of every pad while the guide menu is open: menu
 * navigation must not leak into the app on screen.  Closing the fd on
 * device removal drops the kernel grab by itself. */
static int pads_grabbed;

void
htpc_pad_grab(int on)
{
	HtpcPad *gp;

	if (!pad_inited || pads_grabbed == !!on)
		return;
	pads_grabbed = !!on;
	wl_list_for_each(gp, &pads_list, link)
		if (gp->fd >= 0)
			ioctl(gp->fd, EVIOCGRAB, on ? (void *)1 : (void *)0);
}

static int
pad_event_cb(int fd, uint32_t mask, void *data)
{
	HtpcPad *gp = data;

	if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
		remove_device(gp);
		reevaluate();
		return 0;
	}

	struct input_event ev;
	ssize_t n;
	while ((n = read(fd, &ev, sizeof(ev))) == (ssize_t)sizeof(ev)) {
		/* Guide menu (htpc_guide.c): guide toggles it; while it is
		 * open — pads grabbed exclusively — the d-pad moves the
		 * selection (button or hat), A selects, B closes.  Shoulder
		 * hold-nav below stays untouched.
		 *
		 * Toggle on RELEASE, not press: Steam shares this evdev fd
		 * and the kernel queues each event to every open client
		 * before our grab lands.  Toggling on press grabbed the pad
		 * between down and up, so Steam saw the down but never the
		 * up — and a held guide button is Big Picture's power-menu
		 * chord.  On release both halves of the press are already in
		 * Steam's queue (a clean short press it ignores with "Guide
		 * Button Focuses Steam" off), and the grab still lands before
		 * any menu navigation. */
		if (ev.type == EV_KEY && ev.code == BTN_MODE) {
			if (ev.value == 0)
				htpc_guide_toggle();
			continue;
		}
		if (htpc_guide_is_open()) {
			if (ev.type == EV_KEY && ev.value == 1) {
				if (ev.code == BTN_DPAD_UP)
					htpc_guide_nav(-1);
				else if (ev.code == BTN_DPAD_DOWN)
					htpc_guide_nav(1);
				else if (ev.code == BTN_SOUTH)
					htpc_guide_select();
				else if (ev.code == BTN_EAST)
					htpc_guide_close();
			} else if (ev.type == EV_ABS &&
					ev.code == ABS_HAT0Y && ev.value != 0) {
				htpc_guide_nav(ev.value < 0 ? -1 : 1);
			}
			continue;
		}
		if (ev.type != EV_KEY)
			continue;
		if (ev.code == BTN_TL)
			gp->l1_down = (ev.value != 0);
		else if (ev.code == BTN_TR)
			gp->r1_down = (ev.value != 0);
		else
			continue;
		reevaluate();
	}
	if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
		remove_device(gp);
		reevaluate();
	}
	return 0;
}

static void
try_add_device(const char *path)
{
	HtpcPad *gp;

	wl_list_for_each(gp, &pads_list, link) {
		if (strcmp(gp->path, path) == 0)
			return;
	}

	int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		return;
	if (!is_gamepad(fd)) {
		close(fd);
		return;
	}

	gp = calloc(1, sizeof(*gp));
	if (!gp) {
		close(fd);
		return;
	}
	gp->fd = fd;
	snprintf(gp->path, sizeof(gp->path), "%s", path);
	gp->src = wl_event_loop_add_fd(event_loop, fd, WL_EVENT_READABLE,
				       pad_event_cb, gp);
	if (!gp->src) {
		close(fd);
		free(gp);
		return;
	}
	wl_list_insert(&pads_list, &gp->link);
	/* Hotplug while the guide menu is open: join the active grab. */
	if (pads_grabbed)
		ioctl(fd, EVIOCGRAB, (void *)1);
	wlr_log(WLR_INFO, "htpc_pad: gamepad added %s", path);
}

static int
inotify_cb(int fd, uint32_t mask, void *data)
{
	(void)mask;
	(void)data;
	char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
	ssize_t n;
	while ((n = read(fd, buf, sizeof(buf))) > 0) {
		char *p = buf;
		while (p < buf + n) {
			struct inotify_event *iev = (struct inotify_event *)p;
			if (iev->len > 0 && strncmp(iev->name, "event", 5) == 0
			    && (iev->mask & (IN_CREATE | IN_ATTRIB))) {
				char path[64];
				snprintf(path, sizeof(path), "/dev/input/%s",
					 iev->name);
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
	if (!d)
		return;
	struct dirent *e;
	while ((e = readdir(d))) {
		if (strncmp(e->d_name, "event", 5) != 0)
			continue;
		char path[64];
		snprintf(path, sizeof(path), "/dev/input/%s", e->d_name);
		try_add_device(path);
	}
	closedir(d);
}

void
htpc_pad_setup(void)
{
	if (pad_inited || !htpc_mode_active)
		return;
	wl_list_init(&pads_list);

	hold_timer = wl_event_loop_add_timer(event_loop, hold_timer_cb, NULL);

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
	wlr_log(WLR_INFO, "htpc_pad: setup done");
}

void
htpc_pad_cleanup(void)
{
	HtpcPad *gp, *tmp;

	if (!pad_inited)
		return;
	htpc_guide_close();
	pads_grabbed = 0;
	wl_list_for_each_safe(gp, tmp, &pads_list, link)
		remove_device(gp);
	if (hold_timer) {
		wl_event_source_remove(hold_timer);
		hold_timer = NULL;
	}
	if (inotify_src) {
		wl_event_source_remove(inotify_src);
		inotify_src = NULL;
	}
	if (inotify_fd >= 0) {
		close(inotify_fd);
		inotify_fd = -1;
	}
	hold_dir = 0;
	pad_inited = 0;
}
