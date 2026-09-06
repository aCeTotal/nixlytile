/* bindings_conf.c — live keybindings from ~/.local/nixlyos/bindings.conf.
 *
 * The file holds native KDL bind lines and is owned by the user (seeded by
 * NixlyOS with the full default set). config_loader.c applies it on every
 * load/reload; the inotify watch here turns a plain save of the file into an
 * immediate reload_config(), so bindings apply without rebuild or relogin.
 *
 * The directory is watched rather than the file: editors replace the inode
 * on save (rename-over), and the dir watch survives that.
 */

#include "nixlytile.h"

#include <errno.h>
#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <unistd.h>

#define BINDINGSCONF_NAME "bindings.conf"

static int bindingsconf_fd = -1;
static struct wl_event_source *bindingsconf_source;

static void
resolve_bindings_dir(char *out, size_t cap)
{
	const char *dir = getenv("NIXLYOS_DIR");
	if (dir && *dir) {
		snprintf(out, cap, "%s", dir);
		return;
	}
	const char *home = getenv("HOME");
	if (!home) {
		struct passwd *pw = getpwuid(getuid());
		if (pw) home = pw->pw_dir;
	}
	if (!home) home = "/";
	snprintf(out, cap, "%s/.local/nixlyos", home);
}

static int
bindingsconf_readable(int fd, uint32_t mask, void *data)
{
	char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
	ssize_t len;
	int relevant = 0;
	(void)mask;
	(void)data;

	while ((len = read(fd, buf, sizeof(buf))) > 0) {
		char *p = buf;
		while (p < buf + len) {
			struct inotify_event *ev = (struct inotify_event *)p;
			if (ev->len && strcmp(ev->name, BINDINGSCONF_NAME) == 0)
				relevant = 1;
			p += sizeof(*ev) + ev->len;
		}
	}

	if (relevant)
		reload_config();
	return 0;
}

void
setup_bindings_conf_watch(void)
{
	char dir[PATH_MAX];
	int wd;

	resolve_bindings_dir(dir, sizeof(dir));

	bindingsconf_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (bindingsconf_fd < 0) {
		wlr_log(WLR_ERROR, "bindings_conf: inotify_init failed: %s",
			strerror(errno));
		return;
	}

	wd = inotify_add_watch(bindingsconf_fd, dir,
		IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE | IN_DELETE_SELF);
	if (wd < 0) {
		wlr_log(WLR_INFO, "bindings_conf: no %s yet (%s)", dir, strerror(errno));
		close(bindingsconf_fd);
		bindingsconf_fd = -1;
		return;
	}

	bindingsconf_source = wl_event_loop_add_fd(event_loop, bindingsconf_fd,
		WL_EVENT_READABLE, bindingsconf_readable, NULL);
	if (!bindingsconf_source) {
		close(bindingsconf_fd);
		bindingsconf_fd = -1;
		return;
	}
	wlr_log(WLR_INFO, "bindings_conf: watching %s/%s", dir, BINDINGSCONF_NAME);
}
