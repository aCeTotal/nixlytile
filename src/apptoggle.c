/*
 * apptoggle.c — gamepad-only toggle between nixlymedia and retroarch.
 *
 * Press L1+R1 simultaneously on any connected gamepad to switch:
 *   running nixlymedia → kill, spawn retroarch
 *   running retroarch  → kill, spawn nixlymedia
 *   neither            → spawn nixlymedia
 *
 * Opens /dev/input/event* devices non-exclusively; inotify picks up
 * hotplugged pads.  1s debounce. Compositor SIGCHLD handler reaps.
 */
#include "nixlytile.h"
#include "client.h"

#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define APPTOGGLE_DEBOUNCE_MS 1000
#define LONGS_FOR(bits) (((bits) + 8 * sizeof(long) - 1) / (8 * sizeof(long)))
#define TESTBIT(b, arr) (((arr)[(b) / (8 * sizeof(long))] >> ((b) % (8 * sizeof(long)))) & 1UL)

typedef enum { APP_NONE, APP_NIXLYMEDIA, APP_RETROARCH } CurrentApp;

typedef struct {
	int fd;
	char path[64];
	struct wl_event_source *src;
	int l1_down;
	int r1_down;
	struct wl_list link;
} GamepadDev;

static struct wl_list gamepads_list;
static int inotify_fd = -1;
static struct wl_event_source *inotify_src;
static CurrentApp current_app = APP_NONE;
static pid_t current_pid = 0;
static uint64_t last_toggle_ms = 0;
static int apptoggle_inited = 0;

static uint64_t
now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static int
is_gamepad(int fd)
{
	unsigned long key_bits[LONGS_FOR(KEY_MAX + 1)];
	memset(key_bits, 0, sizeof(key_bits));
	if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits) < 0)
		return 0;
	/* Real gamepads expose BTN_GAMEPAD (== BTN_SOUTH) plus L1/R1.
	 * Keyboards/mice never do. */
	return TESTBIT(BTN_GAMEPAD, key_bits)
		&& TESTBIT(BTN_TL, key_bits)
		&& TESTBIT(BTN_TR, key_bits);
}

static void
remove_device(GamepadDev *gp)
{
	if (gp->src)
		wl_event_source_remove(gp->src);
	if (gp->fd >= 0)
		close(gp->fd);
	wl_list_remove(&gp->link);
	free(gp);
}

static void
perform_toggle(void)
{
	uint64_t now = now_ms();
	if (now - last_toggle_ms < APPTOGGLE_DEBOUNCE_MS)
		return;
	last_toggle_ms = now;

	/* Drop tracked pid if process is gone. */
	if (current_pid > 0 && kill(current_pid, 0) != 0) {
		current_pid = 0;
		current_app = APP_NONE;
	}

	const char *next_cmd;
	CurrentApp next_app;
	if (current_app == APP_RETROARCH) {
		next_cmd = "nixlymedia";
		next_app = APP_NIXLYMEDIA;
	} else if (current_app == APP_NIXLYMEDIA) {
		next_cmd = "retroarch";
		next_app = APP_RETROARCH;
	} else {
		next_cmd = "nixlymedia";
		next_app = APP_NIXLYMEDIA;
	}

	if (current_pid > 0) {
		kill(current_pid, SIGKILL);
		current_pid = 0;
	}
	current_app = APP_NONE;

	pid_t pid = spawn_cmd(next_cmd);
	if (pid > 0) {
		current_pid = pid;
		current_app = next_app;
	}
	wlr_log(WLR_INFO, "apptoggle: spawn '%s' pid=%d", next_cmd, (int)pid);
}

static int
gamepad_event_cb(int fd, uint32_t mask, void *data)
{
	GamepadDev *gp = data;

	if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
		wlr_log(WLR_INFO, "apptoggle: gamepad gone %s", gp->path);
		remove_device(gp);
		return 0;
	}

	struct input_event ev;
	ssize_t n;
	while ((n = read(fd, &ev, sizeof(ev))) == (ssize_t)sizeof(ev)) {
		if (ev.type != EV_KEY)
			continue;
		if (ev.code == BTN_TL)
			gp->l1_down = (ev.value != 0);
		else if (ev.code == BTN_TR)
			gp->r1_down = (ev.value != 0);
		else
			continue;
		if (gp->l1_down && gp->r1_down)
			perform_toggle();
	}
	if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
		wlr_log(WLR_INFO, "apptoggle: read err on %s, dropping", gp->path);
		remove_device(gp);
	}
	return 0;
}

static void
try_add_device(const char *path)
{
	GamepadDev *gp;

	wl_list_for_each(gp, &gamepads_list, link) {
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
				       gamepad_event_cb, gp);
	if (!gp->src) {
		close(fd);
		free(gp);
		return;
	}
	wl_list_insert(&gamepads_list, &gp->link);
	wlr_log(WLR_INFO, "apptoggle: gamepad added %s", path);
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
apptoggle_setup(void)
{
	if (apptoggle_inited)
		return;
	wl_list_init(&gamepads_list);

	/* Adopt the nixlymedia pid spawned by autostart so the first
	 * shoulder-press kills it rather than just stacking retroarch. */
	for (size_t i = 0; i < runtime_autostart_count; i++) {
		if (runtime_autostart[i]
		    && strstr(runtime_autostart[i], "nixlymedia")
		    && runtime_autostart_pids[i] > 0) {
			current_pid = runtime_autostart_pids[i];
			current_app = APP_NIXLYMEDIA;
			break;
		}
	}

	inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (inotify_fd >= 0) {
		inotify_add_watch(inotify_fd, "/dev/input",
				  IN_CREATE | IN_ATTRIB | IN_DELETE);
		inotify_src = wl_event_loop_add_fd(event_loop, inotify_fd,
						   WL_EVENT_READABLE,
						   inotify_cb, NULL);
	}
	scan_devices();
	apptoggle_inited = 1;
	wlr_log(WLR_INFO, "apptoggle: setup done (tracked pid=%d app=%d)",
		(int)current_pid, (int)current_app);
}

void
apptoggle_cleanup(void)
{
	GamepadDev *gp, *tmp;
	wl_list_for_each_safe(gp, tmp, &gamepads_list, link)
		remove_device(gp);
	if (inotify_src) {
		wl_event_source_remove(inotify_src);
		inotify_src = NULL;
	}
	if (inotify_fd >= 0) {
		close(inotify_fd);
		inotify_fd = -1;
	}
	apptoggle_inited = 0;
}
