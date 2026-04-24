#include "nixlytile.h"
#include "client.h"

int
is_process_running(const char *name)
{
	DIR *dir;
	struct dirent *ent;
	char path[PATH_MAX];
	char comm[256];
	FILE *fp;
	int found = 0;

	if (!name || !name[0])
		return 0;

	dir = opendir("/proc");
	if (!dir)
		return 0;

	while ((ent = readdir(dir))) {
		/* Skip non-numeric entries */
		if (ent->d_name[0] < '0' || ent->d_name[0] > '9')
			continue;

		snprintf(path, sizeof(path), "/proc/%s/comm", ent->d_name);
		fp = fopen(path, "r");
		if (!fp)
			continue;

		if (fgets(comm, sizeof(comm), fp)) {
			/* Remove trailing newline */
			char *nl = strchr(comm, '\n');
			if (nl)
				*nl = '\0';
			if (strcasecmp(comm, name) == 0 || strcasestr(comm, name)) {
				found = 1;
				fclose(fp);
				break;
			}
		}
		fclose(fp);
	}

	closedir(dir);
	return found;
}

Client *
find_client_by_app_id(const char *app_id)
{
	Client *c;

	if (!app_id || !app_id[0])
		return NULL;

	wl_list_for_each(c, &clients, link) {
		if (c->type == XDGShell && c->surface.xdg && c->surface.xdg->toplevel) {
			const char *cid = c->surface.xdg->toplevel->app_id;
			if (cid && (strcasecmp(cid, app_id) == 0 || strcasestr(cid, app_id)))
				return c;
		}
#ifdef XWAYLAND
		if (c->type == X11 && c->surface.xwayland) {
			const char *cls = c->surface.xwayland->class;
			if (cls && (strcasecmp(cls, app_id) == 0 || strcasestr(cls, app_id)))
				return c;
		}
#endif
	}
	return NULL;
}

Client *
find_discord_client(void)
{
	Client *c;
	const char *discord_ids[] = {
		"discord", "Discord", ".Discord", ".Discord-wrapped",
		"vesktop", "Vesktop", "webcord", "WebCord",
		"armcord", "ArmCord", "legcord", "Legcord",
		NULL
	};

	wl_list_for_each(c, &clients, link) {
		const char *cid = NULL;

		if (c->type == XDGShell && c->surface.xdg && c->surface.xdg->toplevel) {
			cid = c->surface.xdg->toplevel->app_id;
		}
#ifdef XWAYLAND
		if (c->type == X11 && c->surface.xwayland) {
			cid = c->surface.xwayland->class;
		}
#endif
		if (!cid)
			continue;

		/* Check exact matches and substring matches */
		for (int i = 0; discord_ids[i]; i++) {
			if (strcasecmp(cid, discord_ids[i]) == 0 ||
			    strcasestr(cid, discord_ids[i]))
				return c;
		}
	}
	return NULL;
}

void
focus_or_launch_app(const char *app_id, const char *launch_cmd)
{
	Client *found;

	if (!app_id || !app_id[0])
		return;

	/* Find the client with matching app_id.
	 * For Discord, use specialized search that checks multiple app_id variants */
	if (strcasecmp(app_id, "discord") == 0)
		found = find_discord_client();
	else
		found = find_client_by_app_id(app_id);

	if (found) {
		/* Focus the client and switch to its tag/workspace */
		Monitor *m = found->mon;
		if (m && found->tags) {
			/* Switch to the tag where this client is */
			unsigned int newtags = found->tags & TAGMASK;
			if (newtags && newtags != m->tagset[m->seltags]) {
				invalidate_video_pacing(m);
				m->tagset[m->seltags] = newtags;
				focusclient(found, 1);
				arrange(m);
			} else {
				focusclient(found, 1);
			}
		}
	} else if (launch_cmd && launch_cmd[0]) {
		/* No window found — launch same way spawn() does: full child
		 * cleanup (stdin, signals, ambient caps, inherited FDs), steam
		 * and dGPU env handled inside spawn_cmd. */
		spawn_cmd(launch_cmd);
	}
}

Client *
get_fullscreen_client(void)
{
	Client *c;
	wl_list_for_each(c, &clients, link) {
		if (c->isfullscreen && client_surface(c)->mapped && VISIBLEON(c, c->mon))
			return c;
	}
	return NULL;
}

int
any_client_fullscreen(void)
{
	return get_fullscreen_client() != NULL;
}

pid_t
client_get_pid(Client *c)
{
	struct wlr_surface *surface;
	struct wl_client *wl_client;
	pid_t pid = 0;
	uid_t uid;
	gid_t gid;

	if (!c)
		return 0;

#ifdef XWAYLAND
	/* For X11 clients, use the real process PID from xwayland surface.
	 * wl_client_get_credentials() returns the Xwayland server PID,
	 * which makes process-based game detection fail for all XWayland apps. */
	if (client_is_x11(c) && c->surface.xwayland && c->surface.xwayland->pid > 0)
		return c->surface.xwayland->pid;
#endif

	surface = client_surface(c);
	if (!surface || !surface->resource)
		return 0;

	wl_client = wl_resource_get_client(surface->resource);
	if (!wl_client)
		return 0;

	wl_client_get_credentials(wl_client, &pid, &uid, &gid);
	return pid;
}

int
is_steam_cmd(const char *cmd)
{
	if (!cmd)
		return 0;
	return strcasestr(cmd, "steam") != NULL;
}

void
steam_launch_bigpicture(void)
{
	pid_t pid;

	/* Check if Steam is already running */
	if (is_process_running("steam")) {
		wlr_log(WLR_INFO, "Steam already running, not launching again");
		return;
	}

	wlr_log(WLR_INFO, "Launching Steam Big Picture mode");

	pid = fork();
	if (pid == 0) {
		setsid();
		fork_detach();
		/* Steam runs inside bwrap FHS sandbox; skip dGPU env
		 * to avoid GL/EGL failures inside the sandbox. */
		{
			const char *steam_bin = "steam";
			if (access("/etc/profiles/per-user/total/bin/nixly_steam", X_OK) == 0)
				steam_bin = "nixly_steam";
			else if (access("/run/current-system/sw/bin/nixly_steam", X_OK) == 0)
				steam_bin = "nixly_steam";
			execlp(steam_bin, steam_bin, "-bigpicture",
				"steam://open/games", (char *)NULL);
		}
		_exit(127);
	}
}

void
steam_kill(void)
{
	pid_t pid;

	if (!is_process_running("steam")) {
		return;
	}

	wlr_log(WLR_INFO, "Killing Steam process");

	/* Use pkill to kill all steam processes */
	pid = fork();
	if (pid == 0) {
		setsid();
		execlp("pkill", "pkill", "-9", "steam", (char *)NULL);
		_exit(127);
	}
}

void
live_tv_kill(void)
{
	pid_t pid;
	int status;

	/* Kill Chrome/Chromium kiosk instances used for streaming (NRK, Netflix, Viaplay, TV2 Play, F1TV) */
	pid = fork();
	if (pid == 0) {
		setsid();
		execlp("pkill", "pkill", "-9", "-f", "(google-chrome|chromium).*--kiosk.*(nrk\\.no|netflix\\.com|viaplay\\.no|tv2\\.no|f1tv)", (char *)NULL);
		_exit(127);
	}
	if (pid > 0)
		waitpid(pid, &status, 0);
}

/*
 * Read Steam-specific X11 properties from an XWayland surface.
 * Steam sets these atoms on game windows:
 *   STEAM_GAME       — Cardinal, value = game AppID (e.g. 393380)
 *   STEAM_OVERLAY    — Cardinal, 1 = Steam in-game overlay
 *   STEAM_BIGPICTURE — Cardinal, 1 = Steam client / Big Picture
 *
 * These are the authoritative signals from Steam.  Gamescope uses
 * the same properties for window classification.
 */
void
read_steam_properties(Client *c)
{
#ifdef XWAYLAND
	xcb_connection_t *xc;
	xcb_get_property_cookie_t cookie;
	xcb_get_property_reply_t *reply;
	xcb_window_t win;

	if (!c || c->type != X11 || !c->surface.xwayland)
		return;

	xc = wlr_xwayland_get_xwm_connection(xwayland);
	if (!xc)
		return;

	win = c->surface.xwayland->window_id;
	if (!win)
		return;

	/* Batch all three requests (async) before fetching replies */
	xcb_get_property_cookie_t csg = {0}, cso = {0}, csb = {0};
	if (atom_steam_game)
		csg = xcb_get_property(xc, 0, win, atom_steam_game,
			XCB_ATOM_CARDINAL, 0, 1);
	if (atom_steam_overlay)
		cso = xcb_get_property(xc, 0, win, atom_steam_overlay,
			XCB_ATOM_CARDINAL, 0, 1);
	if (atom_steam_bigpicture)
		csb = xcb_get_property(xc, 0, win, atom_steam_bigpicture,
			XCB_ATOM_CARDINAL, 0, 1);

	/* STEAM_GAME → steam_game_id */
	if (atom_steam_game) {
		reply = xcb_get_property_reply(xc, csg, NULL);
		if (reply) {
			if (reply->type == XCB_ATOM_CARDINAL
					&& xcb_get_property_value_length(reply) >= 4) {
				c->steam_game_id =
					*(uint32_t *)xcb_get_property_value(reply);
			}
			free(reply);
		}
	}

	/* STEAM_OVERLAY */
	if (atom_steam_overlay) {
		reply = xcb_get_property_reply(xc, cso, NULL);
		if (reply) {
			if (reply->type == XCB_ATOM_CARDINAL
					&& xcb_get_property_value_length(reply) >= 4) {
				c->is_steam_overlay =
					*(uint32_t *)xcb_get_property_value(reply) != 0;
			}
			free(reply);
		}
	}

	/* STEAM_BIGPICTURE */
	if (atom_steam_bigpicture) {
		reply = xcb_get_property_reply(xc, csb, NULL);
		if (reply) {
			if (reply->type == XCB_ATOM_CARDINAL
					&& xcb_get_property_value_length(reply) >= 4) {
				c->is_steam_bigpicture =
					*(uint32_t *)xcb_get_property_value(reply) != 0;
			}
			free(reply);
		}
	}

	if (c->steam_game_id || c->is_steam_overlay || c->is_steam_bigpicture) {
		wlr_log(WLR_INFO,
			"Steam X11 props: appid='%s' STEAM_GAME=%u "
			"OVERLAY=%d BIGPICTURE=%d",
			client_get_appid(c) ? client_get_appid(c) : "(null)",
			c->steam_game_id, c->is_steam_overlay,
			c->is_steam_bigpicture);
	}
#endif
}

int
is_steam_client(Client *c)
{
	const char *app_id;
	if (!c)
		return 0;
#ifdef XWAYLAND
	if (c->is_steam_bigpicture)
		return 1;
#endif
	app_id = client_get_appid(c);
	if (!app_id)
		return 0;
	/* Steam main window app_id is "steam" */
	return strcasecmp(app_id, "steam") == 0;
}

int
is_steam_popup(Client *c)
{
	const char *app_id;
	if (!c)
		return 0;
#ifdef XWAYLAND
	/* STEAM_OVERLAY is a definitive "not a game" signal */
	if (c->is_steam_overlay)
		return 1;
	/* STEAM_GAME with a game AppID → not a popup */
	if (c->steam_game_id > 0 && c->steam_game_id != 769)
		return 0;
#endif
	app_id = client_get_appid(c);
	if (!app_id)
		return 0;
	/* Steam dialogs/popups — but NOT games with steam_app_XXXX class.
	 * Proton/Wine games typically get WM_CLASS "steam_app_12345". */
	if (strncasecmp(app_id, "steam_app_", 10) == 0)
		return 0;  /* This is a game, not a popup */
	if (strcasestr(app_id, "steam") || strcasestr(app_id, "steamwebhelper"))
		return 1;
	return 0;
}

static int
is_wine_or_proton_process(pid_t pid)
{
	char path[64], target[512];
	ssize_t len;

	if (pid <= 1)
		return 0;

	snprintf(path, sizeof(path), "/proc/%d/exe", pid);
	len = readlink(path, target, sizeof(target) - 1);
	if (len <= 0)
		return 0;
	target[len] = '\0';

	if (strstr(target, "wine-preloader") ||
	    strstr(target, "wine64-preloader") ||
	    strstr(target, "wineserver") ||
	    strstr(target, "/proton") ||
	    strstr(target, "/wine"))
		return 1;

	return 0;
}

static int
is_known_game_app(const char *app)
{
	static const char *game_apps[] = {
		/* Emulators are intentionally NOT listed — the retro-gaming
		 * page handles them explicitly and the user does not want
		 * game mode to engage for them (no process freeze, no GPU
		 * clock lock, no statusbar hide). */
		/* Game launchers / wrappers */
		"gamescope", "heroic", "lutris", "bottles",
		"net.lutris.Lutris", "com.heroicgameslauncher.hgl",
		/* Known games */
		"minecraft", "Minecraft",
		"com.mojang.minecraft",
		NULL
	};

	if (!app)
		return 0;

	for (int i = 0; game_apps[i]; i++) {
		if (strcasecmp(app, game_apps[i]) == 0)
			return 1;
	}
	return 0;
}

int
is_game_launcher_child(pid_t pid)
{
	char path[64], comm[64];
	FILE *f;
	pid_t ppid;
	int depth = 0;
	const int max_depth = 10; /* Prevent infinite loops */

	if (pid <= 1)
		return 0;

	/* Read the initial process's ppid, but do NOT test its own comm.
	 * Otherwise the Steam main window (comm="steam") matches itself. */
	snprintf(path, sizeof(path), "/proc/%d/stat", pid);
	f = fopen(path, "r");
	if (!f)
		return 0;
	if (fscanf(f, "%*d (%63[^)]) %*c %d", comm, &ppid) != 2) {
		fclose(f);
		return 0;
	}
	fclose(f);

	pid = ppid;
	while (pid > 1 && depth < max_depth) {
		snprintf(path, sizeof(path), "/proc/%d/stat", pid);
		f = fopen(path, "r");
		if (!f)
			return 0;

		/* Format: pid (comm) state ppid ... */
		if (fscanf(f, "%*d (%63[^)]) %*c %d", comm, &ppid) != 2) {
			fclose(f);
			return 0;
		}
		fclose(f);

		/* Steam-only ancestry: steam client itself or reaper (Steam's
		 * per-game wrapper). Lutris/Heroic/Bottles deliberately excluded —
		 * gamemode must ONLY engage for Steam-launched games. */
		if (strcasestr(comm, "steam") ||
		    strcasestr(comm, "reaper"))
			return 1;

		pid = ppid;
		depth++;
	}
	return 0;
}

int
is_steam_game(Client *c)
{
	const char *app_id;
	pid_t pid;

	if (!c)
		return 0;

#ifdef XWAYLAND
	/* Authoritative: STEAM_GAME atom set by Steam (non-zero, not 769=Steam) */
	if (c->steam_game_id > 0 && c->steam_game_id != 769
			&& !c->is_steam_overlay)
		return 1;
#endif

	/* Exclude Steam itself and its popups, but NOT games with steam_app_XXXX class */
	app_id = client_get_appid(c);
	if (app_id && (strcasecmp(app_id, "steam") == 0 ||
	    strcasecmp(app_id, "Steam") == 0 ||
	    strcasestr(app_id, "steamwebhelper")))
		return 0;

	/* Exclude floating windows (likely dialogs/launchers) */
	if (c->isfloating)
		return 0;

	/* Check if it's a child of Steam */
	pid = client_get_pid(c);
	if (pid > 1 && is_game_launcher_child(pid)) {
		wlr_log(WLR_INFO, "Detected Steam game: app_id='%s', pid=%d",
			app_id ? app_id : "(null)", pid);
		return 1;
	}

	return 0;
}

int
is_browser_client(Client *c)
{
	const char *app_id;
	if (!c)
		return 0;
	app_id = client_get_appid(c);
	if (!app_id)
		return 0;
	/* Check for common browser app_ids */
	if (strcasestr(app_id, "chrome") ||
	    strcasestr(app_id, "chromium") ||
	    strcasestr(app_id, "google-chrome") ||
	    strcasestr(app_id, "firefox") ||
	    strcasestr(app_id, "brave") ||
	    strcasestr(app_id, "vivaldi") ||
	    strcasestr(app_id, "opera") ||
	    strcasestr(app_id, "edge"))
		return 1;
	return 0;
}

int
looks_like_game(Client *c)
{
	const char *app;
	pid_t pid;

	if (!c)
		return 0;

#ifdef XWAYLAND
	/* Authoritative: Steam overlay windows are never games */
	if (c->is_steam_overlay)
		return 0;

	/* Authoritative: STEAM_GAME atom set by Steam (non-zero, not 769) */
	if (c->steam_game_id > 0 && c->steam_game_id != 769)
		return 1;
#endif

	/* Retro emulators are explicitly excluded from game-mode treatment.
	 * Must be checked before the protocol-hint branches because RetroArch
	 * announces content-type=game on its Wayland surface. */
	if (is_retro_emulator_client(c))
		return 0;

	/* Steam main window / popups are never games. Must be checked before
	 * process-ancestry detection, which otherwise matches the Steam
	 * launcher itself via its own comm. */
	if (is_steam_client(c) || is_steam_popup(c))
		return 0;

	/* Protocol hints */
	if (is_game_content(c))
		return 1;
	if (client_wants_tearing(c))
		return 1;

	/* Check if it's a Steam game (app-id or process ancestry) */
	if (is_steam_game(c))
		return 1;

	/* App-ID whitelist */
	app = client_get_appid(c);
	if (app && strncasecmp(app, "steam_app_", 10) == 0)
		return 1;
	if (app && is_known_game_app(app))
		return 1;

	/* Process-based detection */
	pid = client_get_pid(c);
	if (pid > 1 && is_wine_or_proton_process(pid))
		return 1;
	if (pid > 1 && is_game_launcher_child(pid))
		return 1;

	return 0;
}

float
ease_out_cubic(float t)
{
	t = t - 1.0f;
	return t * t * t + 1.0f;
}
