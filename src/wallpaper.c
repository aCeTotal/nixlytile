#include "nixlytile.h"
#include "config_loader.h"

#include <spawn.h>
#include <sys/inotify.h>

#define SETTLE_MS 50
#define SWAP_MS 1000
#define WATCH_MASK (IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE | IN_ATTRIB)

extern char **environ;

typedef struct {
	int wd;
	char name[NAME_MAX + 1];
} Watch;

static pid_t swaybg_pid = -1, retired_pid = -1;
static char shown_path[PATH_MAX];
static int watch_fd = -1, config_changed;
static Watch config_watch = { -1, "" }, wall_watch = { -1, "" };
static struct wl_event_source *settle_timer, *swap_timer;

static int
retire_old(void *data)
{
	(void)data;
	if (retired_pid > 0)
		kill(retired_pid, SIGTERM);
	retired_pid = -1;
	return 0;
}

/* New swaybg first; the old one leaves once it drew. */
static void
show(void)
{
	char *argv[] = { "swaybg", "-i", wallpaper_path, "-m", "fill", NULL };
	posix_spawnattr_t attr;
	pid_t pid;

	posix_spawnattr_init(&attr);
	posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID);
	if (posix_spawnp(&pid, argv[0], NULL, &attr, argv, environ) == 0) {
		retire_old(NULL);
		retired_pid = swaybg_pid;
		swaybg_pid = pid;
		wl_event_source_timer_update(swap_timer, SWAP_MS);
	}
	posix_spawnattr_destroy(&attr);
	snprintf(shown_path, sizeof(shown_path), "%s", wallpaper_path);
	nx_log(NX_INFO, "wallpaper: %s", wallpaper_path);
	theme_wallpaper_update();
}

static void
watch_file(Watch *w, const char *path)
{
	char dir[PATH_MAX];
	const char *slash = strrchr(path, '/');

	if (!slash || slash == path)
		return;
	snprintf(dir, sizeof(dir), "%.*s", (int)(slash - path), path);
	snprintf(w->name, sizeof(w->name), "%s", slash + 1);
	w->wd = inotify_add_watch(watch_fd, dir, WATCH_MASK);
}

static int
matches(const Watch *w, const struct inotify_event *ev)
{
	return ev->wd == w->wd && ev->len && !strcmp(ev->name, w->name);
}

static int
settled(void *data)
{
	(void)data;
	if (config_changed)
		reload_config();
	else
		show();
	config_changed = 0;
	return 0;
}

static int
scan_events(const char *buf, ssize_t len)
{
	const struct inotify_event *ev;
	int hit = 0;

	for (const char *p = buf; p < buf + len; p += sizeof(*ev) + ev->len) {
		ev = (const struct inotify_event *)p;
		config_changed |= matches(&config_watch, ev);
		hit |= config_changed || matches(&wall_watch, ev);
	}
	return hit;
}

/* Coalesce bursts from home-manager activation. */
static int
watch_readable(int fd, uint32_t mask, void *data)
{
	char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
	ssize_t len;
	int hit = 0;

	(void)mask;
	(void)data;
	while ((len = read(fd, buf, sizeof(buf))) > 0)
		hit |= scan_events(buf, len);
	if (hit)
		wl_event_source_timer_update(settle_timer, SETTLE_MS);
	return 0;
}

static int
ensure_watch(void)
{
	if (watch_fd >= 0)
		return 1;
	watch_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (watch_fd < 0)
		return 0;
	wl_event_loop_add_fd(event_loop, watch_fd, WL_EVENT_READABLE,
			watch_readable, NULL);
	settle_timer = wl_event_loop_add_timer(event_loop, settled, NULL);
	swap_timer = wl_event_loop_add_timer(event_loop, retire_old, NULL);
	return 1;
}

/* Show the configured wallpaper and track edits. */
void
wallpaper_sync(void)
{
	if (htpc_mode_active || !wallpaper_path[0] || !ensure_watch())
		return;
	watch_file(&config_watch, nixlytile_config_path);
	watch_file(&wall_watch, wallpaper_path);
	if (strcmp(shown_path, wallpaper_path))
		show();
}
