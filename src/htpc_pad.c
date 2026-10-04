/* Guide button drives the guide menu. */
#include "nixlytile.h"
#include "client.h"

#include <dirent.h>
#include <fcntl.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define LONGS_FOR(bits) (((bits) + 8 * sizeof(long) - 1) / (8 * sizeof(long)))
#define TESTBIT(b, arr) (((arr)[(b) / (8 * sizeof(long))] >> ((b) % (8 * sizeof(long)))) & 1UL)

typedef struct {
	int fd;
	char path[64];
	struct wl_event_source *src;
	struct wl_list link;
} HtpcPad;

static struct wl_list pads_list;
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
remove_device(HtpcPad *gp)
{
	if (gp->src)
		wl_event_source_remove(gp->src);
	if (gp->fd >= 0)
		close(gp->fd);
	wl_list_remove(&gp->link);
	free(gp);
}

/* Grab pads while guide open. */
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

static void
guide_input(const struct input_event *ev)
{
	if (ev->type == EV_ABS && ev->code == ABS_HAT0Y && ev->value != 0) {
		htpc_guide_nav(ev->value < 0 ? -1 : 1);
		return;
	}
	if (ev->type != EV_KEY || ev->value != 1)
		return;
	switch (ev->code) {
	case BTN_DPAD_UP:
		htpc_guide_nav(-1);
		break;
	case BTN_DPAD_DOWN:
		htpc_guide_nav(1);
		break;
	case BTN_SOUTH:
		htpc_guide_select();
		break;
	case BTN_EAST:
		htpc_guide_close();
		break;
	}
}

static int
pad_event_cb(int fd, uint32_t mask, void *data)
{
	HtpcPad *gp = data;

	if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
		remove_device(gp);
		return 0;
	}

	struct input_event ev;
	ssize_t n;
	while ((n = read(fd, &ev, sizeof(ev))) == (ssize_t)sizeof(ev)) {
		/* Toggle on release; Steam shares fd. */
		if (ev.type == EV_KEY && ev.code == BTN_MODE) {
			if (ev.value == 0)
				htpc_guide_toggle();
			continue;
		}
		if (htpc_guide_is_open())
			guide_input(&ev);
	}
	if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK))
		remove_device(gp);
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
		if (snprintf(path, sizeof(path), "/dev/input/%s", e->d_name)
				>= (int)sizeof(path))
			continue;
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
