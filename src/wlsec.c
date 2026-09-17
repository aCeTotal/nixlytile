/*
 * Global filter for the privileged Wayland protocols.
 *
 * screencopy, image-copy-capture, export-dmabuf, both data-control
 * versions, virtual keyboard/pointer, gamma and output power, the
 * foreign-toplevel list, shortcut inhibition and session lock all let a
 * client watch or drive the whole session.  Unfiltered, every game, wine
 * process and browser tab can record the screen, read the clipboard and
 * type.  Here they are advertised only to binaries on the allowlist, and
 * never to a client inside a sandbox security context.
 *
 * Allowlist: the built-in set plus one basename per line in
 * ~/.local/nixlyos/wayland-trusted.conf.
 */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_security_context_v1.h>

#include "nixlytile.h"

#define WLSEC_MAX_GLOBALS 24
#define WLSEC_MAX_NAMES   32
#define WLSEC_CACHE       64

static const struct wl_global *wlsec_globals[WLSEC_MAX_GLOBALS];
static int wlsec_nglobals;

static char wlsec_names[WLSEC_MAX_NAMES][64];
static int wlsec_nnames;

/* Cached verdict per client, dropped when the client goes away so a reused
 * wl_client address can never inherit someone else's verdict. */
struct WlsecEntry {
	struct wl_listener destroy;
	const struct wl_client *client;
	int allowed;
};
static struct WlsecEntry wlsec_cache[WLSEC_CACHE];

/* Screen capture, clipboard history and the lock screen — the programs
 * that cannot do their job without a privileged protocol. */
static const char *wlsec_builtin[] = {
	"nixlytile",
	"nixly-lockscreen",
	"xdg-desktop-portal-wlr",
	"grim",
	"slurp",
	"wf-recorder",
	"obs",
	"wl-copy",
	"wl-paste",
	"clipman",
	NULL,
};

static void
wlsec_add_name(const char *name)
{
	int i;

	if (!name || !*name || wlsec_nnames >= WLSEC_MAX_NAMES)
		return;
	for (i = 0; i < wlsec_nnames; i++)
		if (strcmp(wlsec_names[i], name) == 0)
			return;
	snprintf(wlsec_names[wlsec_nnames], sizeof(wlsec_names[0]), "%s", name);
	wlsec_nnames++;
}

static void
wlsec_load_names(void)
{
	const char *home = getenv("HOME");
	char path[PATH_MAX], line[80];
	FILE *f;
	int i;

	for (i = 0; wlsec_builtin[i]; i++)
		wlsec_add_name(wlsec_builtin[i]);
	if (!home)
		return;
	snprintf(path, sizeof(path), "%s/.local/nixlyos/wayland-trusted.conf",
			home);
	f = fopen(path, "r");
	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		char *p = line, *end;

		while (*p == ' ' || *p == '\t')
			p++;
		if (*p == '#' || *p == '\n' || *p == '\0')
			continue;
		end = p + strlen(p);
		while (end > p && (end[-1] == '\n' || end[-1] == '\r'
				|| end[-1] == ' ' || end[-1] == '\t'))
			end--;
		*end = '\0';
		wlsec_add_name(p);
	}
	fclose(f);
}

void
wlsec_privileged(struct wl_global *global)
{
	if (global && wlsec_nglobals < WLSEC_MAX_GLOBALS)
		wlsec_globals[wlsec_nglobals++] = global;
}

static int
wlsec_is_privileged(const struct wl_global *global)
{
	int i;

	for (i = 0; i < wlsec_nglobals; i++)
		if (wlsec_globals[i] == global)
			return 1;
	return 0;
}

/* Basename of the client's executable, from its pid. */
static int
wlsec_exe_allowed(pid_t pid)
{
	char link[64], exe[PATH_MAX];
	const char *base;
	ssize_t n;
	int i;

	snprintf(link, sizeof(link), "/proc/%d/exe", (int)pid);
	n = readlink(link, exe, sizeof(exe) - 1);
	if (n <= 0)
		return 0;
	exe[n] = '\0';
	base = strrchr(exe, '/');
	base = base ? base + 1 : exe;
	/* Nix store names are hashed store paths, never the bare binary, so
	 * compare the basename only. */
	for (i = 0; i < wlsec_nnames; i++)
		if (strcmp(wlsec_names[i], base) == 0)
			return 1;
	return 0;
}

static void
wlsec_client_gone(struct wl_listener *listener, void *data)
{
	struct WlsecEntry *e = wl_container_of(listener, e, destroy);

	(void)data;
	wl_list_remove(&listener->link);
	e->client = NULL;
}

static int
wlsec_client_allowed(const struct wl_client *client)
{
	pid_t pid = 0;
	uid_t uid = 0;
	gid_t gid = 0;
	int allowed, i, slot = -1;

	for (i = 0; i < WLSEC_CACHE; i++) {
		if (wlsec_cache[i].client == client)
			return wlsec_cache[i].allowed;
		if (!wlsec_cache[i].client && slot < 0)
			slot = i;
	}

	/* A sandboxed client is never trusted, whatever binary it runs. */
	if (security_ctx_mgr && wlr_security_context_manager_v1_lookup_client(
			security_ctx_mgr, client)) {
		allowed = 0;
	} else {
		wl_client_get_credentials((struct wl_client *)client, &pid, &uid,
				&gid);
		allowed = pid ? wlsec_exe_allowed(pid) : 0;
	}

	if (slot >= 0) {
		wlsec_cache[slot].client = client;
		wlsec_cache[slot].allowed = allowed;
		wlsec_cache[slot].destroy.notify = wlsec_client_gone;
		wl_client_add_destroy_listener((struct wl_client *)client,
				&wlsec_cache[slot].destroy);
	}
	return allowed;
}

static bool
wlsec_filter(const struct wl_client *client, const struct wl_global *global,
		void *data)
{
	(void)data;
	if (!wlsec_is_privileged(global))
		return true;
	return wlsec_client_allowed(client) != 0;
}

void
wlsec_init(void)
{
	wlsec_load_names();
	wl_display_set_global_filter(dpy, wlsec_filter, NULL);
}
