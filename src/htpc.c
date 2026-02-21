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
				m->tagset[m->seltags] = newtags;
				focusclient(found, 1);
				arrange(m);
			} else {
				focusclient(found, 1);
			}
		}
	} else if (launch_cmd && launch_cmd[0]) {
		/* No window found - launch the app (same as launcher) */
		pid_t pid = fork();
		if (pid == 0) {
			dup2(STDERR_FILENO, STDOUT_FILENO);
			setsid();
			if (should_use_dgpu(launch_cmd))
				set_dgpu_env();
			if (is_steam_cmd(launch_cmd)) {
				set_steam_env();
				/* Launch Steam - Big Picture in HTPC mode, normal otherwise */
				if (htpc_mode_active) {
					execlp("steam", "steam", "-bigpicture", "-cef-force-gpu", "-cef-disable-sandbox", "steam://open/games", (char *)NULL);
				} else {
					execlp("steam", "steam", "-cef-force-gpu", "-cef-disable-sandbox", (char *)NULL);
				}
				_exit(1);
			}
			execl("/bin/sh", "sh", "-c", launch_cmd, NULL);
			_exit(127);
		}
	}
}

int
game_refocus_timer_cb(void *data)
{
	Client *c = game_refocus_client;
	(void)data;

	game_refocus_client = NULL;

	if (!c || !client_surface(c) || !client_surface(c)->mapped)
		return 0;

	/* Only refocus if this client is still the focused client */
	if (seat->keyboard_state.focused_surface != client_surface(c))
		return 0;

	wlr_log(WLR_INFO, "game_refocus: re-focusing game '%s' after delay",
		client_get_appid(c) ? client_get_appid(c) : "(unknown)");

	/* Re-send keyboard enter to ensure the client has focus */
	struct wlr_keyboard *kb = wlr_seat_get_keyboard(seat);
	client_notify_enter(client_surface(c), kb);
	client_activate_surface(client_surface(c), 1);

#ifdef XWAYLAND
	/* For XWayland clients, also re-activate the X11 surface */
	if (c->type == X11 && c->surface.xwayland) {
		wlr_xwayland_surface_activate(c->surface.xwayland, 1);
		wlr_xwayland_surface_restack(c->surface.xwayland, NULL, XCB_STACK_MODE_ABOVE);
	}
#endif

	return 0;
}

void
schedule_game_refocus(Client *c, uint32_t ms)
{
	if (!event_loop || !c)
		return;
	if (!game_refocus_timer)
		game_refocus_timer = wl_event_loop_add_timer(event_loop,
				game_refocus_timer_cb, NULL);
	if (game_refocus_timer) {
		game_refocus_client = c;
		wl_event_source_timer_update(game_refocus_timer, ms);
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

	surface = client_surface(c);
	if (!surface || !surface->resource)
		return 0;

	wl_client = wl_resource_get_client(surface->resource);
	if (!wl_client)
		return 0;

	wl_client_get_credentials(wl_client, &pid, &uid, &gid);
	return pid;
}

/*
 * ioprio constants (not always available in headers)
 */
#ifndef IOPRIO_CLASS_RT
#define IOPRIO_CLASS_RT		1
#define IOPRIO_CLASS_BE		2
#define IOPRIO_WHO_PROCESS	1
#define IOPRIO_PRIO_VALUE(class, data)	(((class) << 13) | (data))
#endif

static void
set_cpu_governor(const char *governor)
{
	DIR *dir;
	struct dirent *ent;
	char path[PATH_MAX];
	FILE *fp;
	int count = 0;

	dir = opendir("/sys/devices/system/cpu");
	if (!dir)
		return;

	while ((ent = readdir(dir))) {
		if (strncmp(ent->d_name, "cpu", 3) != 0)
			continue;
		if (ent->d_name[3] < '0' || ent->d_name[3] > '9')
			continue;

		snprintf(path, sizeof(path),
			"/sys/devices/system/cpu/%s/cpufreq/scaling_governor",
			ent->d_name);
		fp = fopen(path, "w");
		if (fp) {
			fprintf(fp, "%s\n", governor);
			fclose(fp);
			count++;
		}
	}
	closedir(dir);

	if (count > 0)
		wlr_log(WLR_INFO, "CPU governor: %s (%d cores)", governor, count);
}

void
apply_game_priority(pid_t pid)
{
	char path[64];
	FILE *fp;

	if (pid <= 1)
		return;

	/*
	 * Set nice value to -10 (requires CAP_SYS_NICE or root).
	 * Stronger than the old -5 for better CPU scheduling priority.
	 */
	if (setpriority(PRIO_PROCESS, pid, -10) == 0) {
		game_mode_nice_applied = 1;
		wlr_log(WLR_INFO, "Game priority: set nice=-10 for PID %d", pid);
	} else {
		wlr_log(WLR_INFO, "Game priority: setpriority failed for PID %d: %s",
			pid, strerror(errno));
	}

	/*
	 * Set I/O scheduling to real-time class (highest priority).
	 * This ensures the game gets the fastest possible disk access.
	 */
	if (syscall(__NR_ioprio_set, IOPRIO_WHO_PROCESS, pid,
		    IOPRIO_PRIO_VALUE(IOPRIO_CLASS_RT, 0)) == 0) {
		game_mode_ioclass_applied = 1;
		wlr_log(WLR_INFO, "Game priority: set ioprio RT class for PID %d", pid);
	} else {
		wlr_log(WLR_INFO, "Game priority: ioprio_set failed for PID %d: %s",
			pid, strerror(errno));
	}

	/*
	 * Protect game from OOM killer by lowering its OOM score.
	 * -500 makes it much less likely to be killed under memory pressure.
	 */
	snprintf(path, sizeof(path), "/proc/%d/oom_score_adj", pid);
	fp = fopen(path, "w");
	if (fp) {
		fprintf(fp, "-500\n");
		fclose(fp);
		game_mode_oom_applied = 1;
		wlr_log(WLR_INFO, "Game priority: set oom_score_adj=-500 for PID %d", pid);
	}

	/*
	 * Set CPU governor to performance for maximum clock speeds.
	 */
	set_cpu_governor("performance");
	game_mode_governor_applied = 1;
}

void
restore_game_priority(pid_t pid)
{
	char path[64];
	FILE *fp;

	if (pid <= 1)
		return;

	/* Restore nice value to 0 (normal) */
	if (game_mode_nice_applied) {
		setpriority(PRIO_PROCESS, pid, 0);
		game_mode_nice_applied = 0;
		wlr_log(WLR_INFO, "Game priority: restored nice=0 for PID %d", pid);
	}

	/* Restore I/O priority to best-effort class, priority 4 (normal) */
	if (game_mode_ioclass_applied) {
		syscall(__NR_ioprio_set, IOPRIO_WHO_PROCESS, pid,
			IOPRIO_PRIO_VALUE(IOPRIO_CLASS_BE, 4));
		game_mode_ioclass_applied = 0;
		wlr_log(WLR_INFO, "Game priority: restored ioprio BE/4 for PID %d", pid);
	}

	/* Restore OOM score to 0 (normal) */
	if (game_mode_oom_applied) {
		snprintf(path, sizeof(path), "/proc/%d/oom_score_adj", pid);
		fp = fopen(path, "w");
		if (fp) {
			fprintf(fp, "0\n");
			fclose(fp);
		}
		game_mode_oom_applied = 0;
		wlr_log(WLR_INFO, "Game priority: restored oom_score_adj=0 for PID %d", pid);
	}

	/* Restore CPU governor to schedutil (energy-efficient default) */
	if (game_mode_governor_applied) {
		set_cpu_governor("schedutil");
		game_mode_governor_applied = 0;
	}
}

int
get_memory_pressure(void)
{
	FILE *f;
	char line[256];
	unsigned long mem_total = 0, mem_available = 0;
	int pressure = 0;

	f = fopen("/proc/meminfo", "r");
	if (!f)
		return 0;

	while (fgets(line, sizeof(line), f)) {
		if (strncmp(line, "MemTotal:", 9) == 0) {
			sscanf(line + 9, "%lu", &mem_total);
		} else if (strncmp(line, "MemAvailable:", 13) == 0) {
			sscanf(line + 13, "%lu", &mem_available);
		}
	}
	fclose(f);

	if (mem_total > 0 && mem_available > 0) {
		/* Calculate percentage of memory in use */
		pressure = 100 - (int)(mem_available * 100 / mem_total);
	}

	return pressure;
}

void
update_game_mode(void)
{
	int was_active = game_mode_active;
	int was_ultra = game_mode_ultra;
	Client *c = get_fullscreen_client();
	Monitor *m;
	int is_game = 0;

	game_mode_active = (c != NULL);
	game_mode_client = c;

	/* Determine if this is actually a game (vs video or other fullscreen content) */
	if (c) {
		is_game = client_wants_tearing(c) || is_game_content(c);
	}

	/* Ultra mode activates for games, regular game mode for other fullscreen content */
	game_mode_ultra = (game_mode_active && is_game);

	if (game_mode_ultra && !was_ultra) {
		/*
		 * ENTERING ULTRA GAME MODE
		 * Maximum performance - stop everything non-essential
		 */
		wlr_log(WLR_INFO, "ULTRA GAME MODE ACTIVATED - maximum performance, minimal latency");

		/* Show "Game detected" notification for 1 second */
		if (c && c->mon)
			toast_show(c->mon, "Game detected", 1000);

		/* Stop ALL background timers to eliminate compositor interrupts */
		if (cache_update_timer)
			wl_event_source_timer_update(cache_update_timer, 0);
		if (nixpkgs_cache_timer)
			wl_event_source_timer_update(nixpkgs_cache_timer, 0);
		if (status_cpu_timer)
			wl_event_source_timer_update(status_cpu_timer, 0);
		if (status_timer)
			wl_event_source_timer_update(status_timer, 0);
		if (status_hover_timer)
			wl_event_source_timer_update(status_hover_timer, 0);
		if (cpu_popup_refresh_timer)
			wl_event_source_timer_update(cpu_popup_refresh_timer, 0);
		if (ram_popup_refresh_timer)
			wl_event_source_timer_update(ram_popup_refresh_timer, 0);
		if (wifi_scan_timer)
			wl_event_source_timer_update(wifi_scan_timer, 0);
		if (video_check_timer)
			wl_event_source_timer_update(video_check_timer, 0);
		if (popup_delay_timer)
			wl_event_source_timer_update(popup_delay_timer, 0);
		if (config_rewatch_timer)
			wl_event_source_timer_update(config_rewatch_timer, 0);
		if (gamepad_inactivity_timer)
			wl_event_source_timer_update(gamepad_inactivity_timer, 0);
		if (gamepad_cursor_timer)
			wl_event_source_timer_update(gamepad_cursor_timer, 0);
		if (hz_osd_timer)
			wl_event_source_timer_update(hz_osd_timer, 0);
		if (pc_gaming_install_timer)
			wl_event_source_timer_update(pc_gaming_install_timer, 0);
		if (playback_osd_timer)
			wl_event_source_timer_update(playback_osd_timer, 0);
		if (media_view_poll_timer)
			wl_event_source_timer_update(media_view_poll_timer, 0);
		if (osk_dpad_repeat_timer)
			wl_event_source_timer_update(osk_dpad_repeat_timer, 0);
		/* NOTE: Keep bt_scan_timer - needed for controller reconnection */
		/* NOTE: Keep gamepad_pending_timer - needed for controller input */

		/* Boost GPU fans to prevent thermal throttling */
		fan_boost_activate();

		/* Hide statusbar on ALL monitors to save GPU compositing time */
		wl_list_for_each(m, &mons, link) {
			m->showbar = 0;
			if (m->statusbar.tree)
				wlr_scene_node_set_enabled(&m->statusbar.tree->node, 0);
		}

		/* Hide all popups and menus - they would just be occluded anyway */
		net_menu_hide_all();
		wifi_popup_hide_all();
		sudo_popup_hide_all();
		tray_menu_hide_all();
		/* Don't hide modal/nixpkgs - user might be searching while game loads */

		/*
		 * Apply high-priority scheduling to the game process.
		 * This gives the game more CPU time and faster I/O access.
		 */
		game_mode_pid = client_get_pid(c);
		if (game_mode_pid > 1) {
			apply_game_priority(game_mode_pid);
		}

		/*
		 * Boost compositor thread to real-time scheduling for lower
		 * input-to-display latency. Priority 2 is low RT (won't starve
		 * audio at priority ~50), but enough to wake promptly for vsync.
		 */
		{
			struct sched_param sp = { .sched_priority = 2 };
			if (sched_setscheduler(0, SCHED_RR, &sp) == 0) {
				compositor_rt_applied = 1;
				wlr_log(WLR_INFO, "Compositor RT priority set (SCHED_RR, prio=2)");
			} else {
				wlr_log(WLR_INFO, "Compositor RT priority failed: %s", strerror(errno));
			}
		}

		/*
		 * Ensure the game client has keyboard focus.
		 * This is critical for games to receive keyboard input immediately.
		 * Use lift=1 to raise to top and warp cursor for immediate input.
		 */
		exclusive_focus = NULL;  /* Clear any exclusive focus that might block */
		focusclient(c, 1);

		/*
		 * Enable game VRR for optimal frame pacing.
		 * This is also done in setfullscreen(), but we need it here too
		 * for when returning to a tag with a fullscreen game.
		 */
		if (c && c->mon && (is_game_content(c) || client_wants_tearing(c))) {
			enable_game_vrr(c->mon);
		}

	} else if (game_mode_ultra && was_ultra && c) {
		/*
		 * RETURNING TO ULTRA GAME MODE (e.g., switching back to tag with fullscreen game)
		 * Focus is now handled automatically by arrange() for all fullscreen clients.
		 * Re-enable VRR in case it was disabled when switching away.
		 */
		if (c->mon && !c->mon->game_vrr_active) {
			enable_game_vrr(c->mon);
		}

	} else if (game_mode_active && !was_active && !game_mode_ultra) {
		/*
		 * ENTERING REGULAR GAME MODE (non-game fullscreen like video)
		 * Less aggressive - just pause some background tasks
		 */
		wlr_log(WLR_INFO, "Game mode activated (non-game fullscreen) - pausing background tasks");

		if (cache_update_timer)
			wl_event_source_timer_update(cache_update_timer, 0);
		if (nixpkgs_cache_timer)
			wl_event_source_timer_update(nixpkgs_cache_timer, 0);
		if (status_cpu_timer)
			wl_event_source_timer_update(status_cpu_timer, 0);

	} else if (!game_mode_active && was_active) {
		/*
		 * EXITING GAME MODE - restore everything
		 */
		wlr_log(WLR_INFO, "Game mode deactivated - restoring normal operation");

		/* Resume all background timers */
		schedule_cache_update_timer();
		schedule_nixpkgs_cache_timer();
		schedule_status_timer();
		schedule_next_status_refresh();

		/* Restore additional timers if we were in ultra mode */
		if (was_ultra) {
			/* Re-enable statusbar on all monitors */
			wl_list_for_each(m, &mons, link) {
				m->showbar = 1;
				if (m->statusbar.tree)
					wlr_scene_node_set_enabled(&m->statusbar.tree->node, 1);
				/* Disable game VRR on all monitors when leaving game mode */
				if (m->game_vrr_active)
					disable_game_vrr(m);
			}

			/* Schedule deferred timer restarts for non-critical tasks */
			if (wifi_scan_timer)
				wl_event_source_timer_update(wifi_scan_timer, 5000);  /* 5s delay */
			if (config_rewatch_timer)
				wl_event_source_timer_update(config_rewatch_timer, 2000);

			/* Restore normal priority for the game process */
			if (game_mode_pid > 1) {
				restore_game_priority(game_mode_pid);
			}

			/* Restore compositor to normal scheduling */
			if (compositor_rt_applied) {
				struct sched_param sp = { .sched_priority = 0 };
				sched_setscheduler(0, SCHED_OTHER, &sp);
				compositor_rt_applied = 0;
				wlr_log(WLR_INFO, "Compositor scheduling restored to SCHED_OTHER");
			}

			/* Restore GPU fans to auto */
			fan_boost_deactivate();

			wlr_log(WLR_INFO, "Ultra game mode deactivated - full system restored");
		}

		game_mode_ultra = 0;
		game_mode_client = NULL;
		game_mode_pid = 0;
	}
}

void
htpc_mode_enter(void)
{
	Monitor *m;
	Client *c, *tmp;

	if (htpc_mode_active)
		return;

	htpc_mode_active = 1;
	wlr_log(WLR_INFO, "HTPC mode activated - killing all clients, minimizing system load");

	/* Close all client windows gracefully */
	wl_list_for_each_safe(c, tmp, &clients, link) {
		client_send_close(c);
	}

	/* Hide statusbar on all monitors */
	wl_list_for_each(m, &mons, link) {
		m->showbar = 0;
		if (m->statusbar.tree)
			wlr_scene_node_set_enabled(&m->statusbar.tree->node, 0);
	}

	/* Hide all status menus and popups */
	net_menu_hide_all();
	wifi_popup_hide_all();
	sudo_popup_hide_all();
	tray_menu_hide_all();
	modal_hide_all();
	nixpkgs_hide_all();
	gamepad_menu_hide_all();
	pc_gaming_hide_all();

	/* Stop ALL background timers to minimize CPU/IO/RAM */
	if (cache_update_timer)
		wl_event_source_timer_update(cache_update_timer, 0);
	if (nixpkgs_cache_timer)
		wl_event_source_timer_update(nixpkgs_cache_timer, 0);
	if (status_cpu_timer)
		wl_event_source_timer_update(status_cpu_timer, 0);
	if (status_timer)
		wl_event_source_timer_update(status_timer, 0);
	if (status_hover_timer)
		wl_event_source_timer_update(status_hover_timer, 0);
	if (cpu_popup_refresh_timer)
		wl_event_source_timer_update(cpu_popup_refresh_timer, 0);
	if (ram_popup_refresh_timer)
		wl_event_source_timer_update(ram_popup_refresh_timer, 0);
	if (wifi_scan_timer)
		wl_event_source_timer_update(wifi_scan_timer, 0);
	/* NOTE: Keep bt_scan_timer running - needed for controller reconnection */
	if (video_check_timer)
		wl_event_source_timer_update(video_check_timer, 0);
	if (popup_delay_timer)
		wl_event_source_timer_update(popup_delay_timer, 0);
	if (config_rewatch_timer)
		wl_event_source_timer_update(config_rewatch_timer, 0);

	/* Start media view poll timer (3 second refresh) */
	if (!media_view_poll_timer)
		media_view_poll_timer = wl_event_loop_add_timer(event_loop, media_view_poll_timer_cb, NULL);
	if (media_view_poll_timer)
		wl_event_source_timer_update(media_view_poll_timer, 3000);

	/* Kill non-essential processes */
	if (fork() == 0) {
		setsid();
		execl("/bin/sh", "sh", "-c",
			"pkill -9 -f 'thunar|nautilus|dolphin' 2>/dev/null; "
			"pkill -9 -f 'code|codium|sublime' 2>/dev/null; "
			"pkill -9 -f 'firefox|chromium|brave|chrome' 2>/dev/null; "
			"pkill -9 -f 'discord|slack|telegram|signal' 2>/dev/null; "
			"pkill -9 -f 'spotify|rhythmbox|vlc' 2>/dev/null;"
			"pkill -9 -f 'gimp|inkscape|blender|kdenlive' 2>/dev/null; "
			"pkill -9 -f 'libreoffice|evince|zathura' 2>/dev/null; "
			"pkill -9 -f 'alacritty|foot|kitty|wezterm|konsole|gnome-terminal' 2>/dev/null; "
			"sync; echo 3 > /proc/sys/vm/drop_caches 2>/dev/null; "
			"echo 10 > /proc/sys/vm/swappiness 2>/dev/null",
			(char *)NULL);
		_exit(0);
	}

	/* Set HTPC wallpaper */
	{
		char expanded_htpc_wp[PATH_MAX];
		config_expand_path(htpc_wallpaper_path, expanded_htpc_wp, sizeof(expanded_htpc_wp));
		wlr_log(WLR_INFO, "HTPC mode: setting wallpaper to %s", expanded_htpc_wp);

		pid_t pid = fork();
		if (pid == 0) {
			setsid();
			/* Kill swaybg, wait, then exec new swaybg directly */
			system("pkill -9 swaybg 2>/dev/null; sleep 0.5");
			execlp("swaybg", "swaybg", "-i", expanded_htpc_wp, "-m", "fill", (char *)NULL);
			_exit(127);
		}
	}

	/* Ensure we start on tag 1 (TV-shows) */
	if (selmon) {
		selmon->seltags ^= 1;
		selmon->tagset[selmon->seltags] = 1 << 0; /* Tag 1 = bit 0 */
		/* Show TV-shows view as default */
		media_view_show(selmon, MEDIA_VIEW_TVSHOWS);
	}

	/* NOTE: Steam is NOT started here - it will be launched on-demand
	 * when the user selects PC-gaming or switches to tag 4 */

	/* Re-arrange all monitors */
	wl_list_for_each(m, &mons, link) {
		arrange(m);
	}

	/* Clear focus since all clients are being closed */
	focusclient(NULL, 0);

	/* Grab gamepads for HTPC UI navigation (unless on Steam tag) */
	gamepad_update_grab_state();
}

void
htpc_mode_exit(void)
{
	Monitor *m;
	Client *c, *tmp;

	if (!htpc_mode_active)
		return;

	htpc_mode_active = 0;
	wlr_log(WLR_INFO, "HTPC mode deactivated - cleaning up, showing statusbar, resuming background tasks");

	/* Stop media view poll timer */
	if (media_view_poll_timer)
		wl_event_source_timer_update(media_view_poll_timer, 0);

	/* Kill Steam and close all clients to get clean tags */
	if (fork() == 0) {
		setsid();
		execl("/bin/sh", "sh", "-c", "pkill -9 steam 2>/dev/null", (char *)NULL);
		_exit(0);
	}

	/* Close all remaining client windows */
	wl_list_for_each_safe(c, tmp, &clients, link) {
		client_send_close(c);
	}

	/* Switch to tag 1 on all monitors */
	wl_list_for_each(m, &mons, link) {
		m->seltags ^= 1;
		m->tagset[m->seltags] = 1 << 0; /* Tag 1 = bit 0 */
	}

	/* Restore normal wallpaper */
	{
		char expanded_wp[PATH_MAX];
		config_expand_path(wallpaper_path, expanded_wp, sizeof(expanded_wp));
		wlr_log(WLR_INFO, "Restoring wallpaper: %s", expanded_wp);

		pid_t pid = fork();
		if (pid == 0) {
			setsid();
			system("pkill -9 swaybg 2>/dev/null; sleep 0.5");
			execlp("swaybg", "swaybg", "-i", expanded_wp, "-m", "fill", (char *)NULL);
			_exit(127);
		}
	}

	/* Show statusbar on all monitors */
	wl_list_for_each(m, &mons, link) {
		m->showbar = 1;
		if (m->statusbar.tree)
			wlr_scene_node_set_enabled(&m->statusbar.tree->node, 1);
	}

	/* Resume background timers (unless game mode is active) */
	if (!game_mode_active) {
		schedule_cache_update_timer();
		schedule_nixpkgs_cache_timer();
		schedule_status_timer();
		schedule_next_status_refresh();
	}

	/* Re-arrange all monitors to allocate statusbar space */
	wl_list_for_each(m, &mons, link) {
		arrange(m);
	}

	/* Release gamepad grab when leaving HTPC mode */
	gamepad_update_grab_state();
}

void
htpc_mode_toggle(const Arg *arg)
{
	(void)arg;
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
		set_dgpu_env();
		set_steam_env();
		execlp("steam", "steam", "-bigpicture", "-cef-force-gpu",
			"-cef-disable-sandbox", "steam://open/games", (char *)NULL);
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

	/* Kill Chrome kiosk instances used for streaming (NRK, Netflix, Viaplay, TV2 Play, F1TV) */
	pid = fork();
	if (pid == 0) {
		setsid();
		execlp("pkill", "pkill", "-9", "-f", "chromium.*--kiosk.*(nrk\\.no|netflix\\.com|viaplay\\.no|tv2\\.no|f1tv)", (char *)NULL);
		_exit(127);
	}
	if (pid > 0)
		waitpid(pid, &status, 0);
}

int
is_steam_client(Client *c)
{
	const char *app_id;
	if (!c)
		return 0;
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
	app_id = client_get_appid(c);
	if (!app_id)
		return 0;
	/* Steam dialogs often have app_id starting with "steam" but may vary */
	if (strcasestr(app_id, "steam"))
		return 1;
	/* Also check for common game launcher popups */
	if (strcasestr(app_id, "steamwebhelper"))
		return 1;
	return 0;
}

int
is_steam_child_process(pid_t pid)
{
	char path[64], buf[512], comm[64];
	FILE *f;
	pid_t ppid;
	int depth = 0;
	const int max_depth = 10; /* Prevent infinite loops */

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

		/* Check if parent is steam, steam.sh, or reaper (Steam's process manager) */
		if (strcasestr(comm, "steam") || strcasestr(comm, "reaper"))
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

	/* Exclude Steam itself and its popups */
	app_id = client_get_appid(c);
	if (app_id && strcasestr(app_id, "steam"))
		return 0;

	/* Exclude floating windows (likely dialogs/launchers) */
	if (c->isfloating)
		return 0;

	/* Check if it's a child of Steam */
	pid = client_get_pid(c);
	if (pid > 1 && is_steam_child_process(pid)) {
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
	if (!c)
		return 0;

	/* Check content-type protocol hint */
	if (is_game_content(c))
		return 1;

	/* Check if it wants tearing (games often request this) */
	if (client_wants_tearing(c))
		return 1;

	/* Check if it's a Steam game */
	if (is_steam_game(c))
		return 1;

	return 0;
}

float
ease_out_cubic(float t)
{
	t = t - 1.0f;
	return t * t * t + 1.0f;
}

int
stats_panel_anim_cb(void *data)
{
	Monitor *m = data;
	uint64_t now_ns, elapsed_ms;
	float progress;
	int new_x;

	if (!m || !m->stats_panel_animating)
		return 0;

	now_ns = get_time_ns();
	elapsed_ms = (now_ns - m->stats_panel_anim_start) / 1000000;

	if (elapsed_ms >= STATS_PANEL_ANIM_DURATION) {
		/* Animation complete */
		m->stats_panel_current_x = m->stats_panel_target_x;
		m->stats_panel_animating = 0;

		/* If we slid out, hide the panel */
		if (m->stats_panel_target_x >= m->m.x + m->m.width) {
			wlr_scene_node_set_enabled(&m->stats_panel_tree->node, 0);
			m->stats_panel_visible = 0;
		}
	} else {
		/* Calculate eased position */
		progress = (float)elapsed_ms / (float)STATS_PANEL_ANIM_DURATION;
		progress = ease_out_cubic(progress);

		/* Determine start position based on target:
		 * If sliding in (target is on-screen), start from off-screen right
		 * If sliding out (target is off-screen), start from on-screen position */
		int off_screen_x = m->m.x + m->m.width;
		int on_screen_x = m->m.x + m->m.width - m->stats_panel_width;
		int sliding_in = (m->stats_panel_target_x < off_screen_x);
		int start_x = sliding_in ? off_screen_x : on_screen_x;
		int end_x = m->stats_panel_target_x;

		new_x = start_x + (int)((float)(end_x - start_x) * progress);
		m->stats_panel_current_x = new_x;

		/* Schedule next frame (~16ms for 60fps animation) */
		wl_event_source_timer_update(m->stats_panel_anim_timer, 16);
	}

	/* Update position */
	if (m->stats_panel_tree)
		wlr_scene_node_set_position(&m->stats_panel_tree->node,
			m->stats_panel_current_x, m->m.y);

	return 0;
}

static int
stats_render_separator(struct wlr_scene_tree *tree, int y, int w,
		int padding, const float color[4])
{
	drawrect(tree, padding, y, w - padding * 2, 1, color);
	return 12;
}

static int
stats_render_section_header(StatusModule *mod, const char *title,
		int y, int line_height, int padding, const float color[4])
{
	char line[128];
	snprintf(line, sizeof(line), "%s", title);
	tray_render_label(mod, line, padding, y + line_height, color);
	return line_height + 4;
}

static int
stats_render_field(StatusModule *mod, const char *label, const char *value,
		int y, int line_height, int padding, int col2_x,
		const float label_color[4], const float value_color[4])
{
	char line[128];
	snprintf(line, sizeof(line), "  %s", label);
	tray_render_label(mod, line, padding, y + line_height, label_color);
	tray_render_label(mod, value, col2_x, y + line_height, value_color);
	return line_height;
}

int
stats_panel_refresh_cb(void *data)
{
	Monitor *m = data;
	float display_hz = 0.0f;
	float native_hz = 0.0f;
	float avg_latency_ms = 0.0f;
	float commit_ms = 0.0f;
	float fps_diff = 0.0f;
	char line[128];
	int y_offset;
	int line_height;
	int padding = 16;
	int col2_x;
	StatusModule mod = {0};
	static const float panel_bg[4] = {0.05f, 0.05f, 0.08f, 0.85f};
	static const float header_color[4] = {0.4f, 0.8f, 1.0f, 1.0f};
	static const float section_color[4] = {0.6f, 0.9f, 1.0f, 1.0f};
	static const float label_color[4] = {0.7f, 0.7f, 0.7f, 1.0f};
	static const float value_color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
	static const float good_color[4] = {0.4f, 1.0f, 0.4f, 1.0f};
	static const float warn_color[4] = {1.0f, 0.8f, 0.2f, 1.0f};
	static const float bad_color[4] = {1.0f, 0.3f, 0.3f, 1.0f};
	static const float sync_color[4] = {0.3f, 1.0f, 0.8f, 1.0f};
	static const float disabled_color[4] = {0.5f, 0.5f, 0.5f, 1.0f};
	struct wlr_scene_node *node, *tmp;
	int is_synced = 0;

	if (!m || !m->stats_panel_visible || !m->stats_panel_tree)
		return 0;

	if (!statusfont.font) {
		wlr_log(WLR_ERROR, "stats_panel_refresh_cb: statusfont not initialized!");
		return 0;
	}

	/* Calculate metrics */
	if (m->present_interval_ns > 0) {
		display_hz = 1000000000.0f / (float)m->present_interval_ns;
	} else if (m->wlr_output->current_mode) {
		display_hz = (float)m->wlr_output->current_mode->refresh / 1000.0f;
	}

	/* Get native/max refresh rate */
	if (m->wlr_output->current_mode) {
		native_hz = (float)m->wlr_output->current_mode->refresh / 1000.0f;
	}

	if (m->frames_presented > 0 && m->total_latency_ns > 0) {
		avg_latency_ms = (float)(m->total_latency_ns / m->frames_presented) / 1000000.0f;
	}

	commit_ms = (float)m->rolling_commit_time_ns / 1000000.0f;

	/* Check if screen and game are synced (within 2 Hz) */
	if (m->estimated_game_fps > 0.0f) {
		fps_diff = fabsf(display_hz - m->estimated_game_fps);
		is_synced = fps_diff < 2.0f;
	}

	/* Clear old content */
	wl_list_for_each_safe(node, tmp, &m->stats_panel_tree->children, link)
		wlr_scene_node_destroy(node);

	/* Draw background */
	drawrect(m->stats_panel_tree, 0, 0, m->stats_panel_width, m->m.height, panel_bg);

	/* Draw left border accent */
	static const float accent[4] = {0.3f, 0.6f, 1.0f, 1.0f};
	drawrect(m->stats_panel_tree, 0, 0, 3, m->m.height, accent);

	line_height = statusfont.height + 8;
	col2_x = m->stats_panel_width / 2;
	y_offset = padding;
	mod.tree = m->stats_panel_tree;

	/* Header */
	snprintf(line, sizeof(line), "PERFORMANCE MONITOR");
	tray_render_label(&mod, line, padding, y_offset + line_height, header_color);
	y_offset += line_height + 8;

	/* Monitor name */
	snprintf(line, sizeof(line), "%s", m->wlr_output->name);
	tray_render_label(&mod, line, padding, y_offset + line_height, label_color);
	y_offset += line_height + 4;

	/* Separator */
	static const float sep_color[4] = {0.3f, 0.3f, 0.4f, 1.0f};
	y_offset += stats_render_separator(m->stats_panel_tree, y_offset,
			m->stats_panel_width, padding, sep_color);

	/* ============ VRR STATUS SECTION ============ */
	y_offset += stats_render_section_header(&mod, "VRR / Adaptive Sync",
			y_offset, line_height, padding, section_color);

	/* VRR Support status */
	snprintf(line, sizeof(line), "  Support:");
	tray_render_label(&mod, line, padding, y_offset + line_height, label_color);
	if (m->vrr_capable) {
		snprintf(line, sizeof(line), "Yes");
		tray_render_label(&mod, line, col2_x, y_offset + line_height, good_color);
	} else {
		snprintf(line, sizeof(line), "No");
		tray_render_label(&mod, line, col2_x, y_offset + line_height, bad_color);
	}
	y_offset += line_height;

	/* VRR Enabled/Disabled status */
	snprintf(line, sizeof(line), "  Setting:");
	tray_render_label(&mod, line, padding, y_offset + line_height, label_color);
	if (!fullscreen_adaptive_sync_enabled) {
		snprintf(line, sizeof(line), "DISABLED");
		tray_render_label(&mod, line, col2_x, y_offset + line_height, bad_color);
	} else if (m->vrr_capable) {
		snprintf(line, sizeof(line), "ENABLED");
		tray_render_label(&mod, line, col2_x, y_offset + line_height, good_color);
	} else {
		snprintf(line, sizeof(line), "N/A");
		tray_render_label(&mod, line, col2_x, y_offset + line_height, disabled_color);
	}
	y_offset += line_height;

	/* VRR Active status */
	snprintf(line, sizeof(line), "  Status:");
	tray_render_label(&mod, line, padding, y_offset + line_height, label_color);
	if (m->game_vrr_active) {
		snprintf(line, sizeof(line), "Game VRR Active");
		tray_render_label(&mod, line, col2_x, y_offset + line_height, sync_color);
	} else if (m->vrr_active) {
		snprintf(line, sizeof(line), "Video VRR Active");
		tray_render_label(&mod, line, col2_x, y_offset + line_height, sync_color);
	} else if (m->vrr_capable && fullscreen_adaptive_sync_enabled) {
		snprintf(line, sizeof(line), "Ready");
		tray_render_label(&mod, line, col2_x, y_offset + line_height, value_color);
	} else {
		snprintf(line, sizeof(line), "Inactive");
		tray_render_label(&mod, line, col2_x, y_offset + line_height, disabled_color);
	}
	y_offset += line_height + 8;

	y_offset += stats_render_separator(m->stats_panel_tree, y_offset,
			m->stats_panel_width, padding, sep_color);

	/* ============ FRAME SMOOTHING SECTION ============ */
	static const float smooth_color[4] = {0.5f, 1.0f, 0.7f, 1.0f};  /* Mint green */
	y_offset += stats_render_section_header(&mod, "Frame Smoothing",
			y_offset, line_height, padding, section_color);

	/* Frame repeat status */
	snprintf(line, sizeof(line), "  Repeat:");
	tray_render_label(&mod, line, padding, y_offset + line_height, label_color);
	if (m->frame_repeat_enabled && m->frame_repeat_count > 1) {
		snprintf(line, sizeof(line), "%dx Active", m->frame_repeat_count);
		tray_render_label(&mod, line, col2_x, y_offset + line_height, smooth_color);
	} else if (m->game_vrr_active) {
		snprintf(line, sizeof(line), "VRR (better)");
		tray_render_label(&mod, line, col2_x, y_offset + line_height, sync_color);
	} else {
		snprintf(line, sizeof(line), "1x (normal)");
		tray_render_label(&mod, line, col2_x, y_offset + line_height, value_color);
	}
	y_offset += line_height;

	/* Show effective frame time when repeat is active */
	if (m->frame_repeat_enabled && m->frame_repeat_count > 1 && display_hz > 0.0f) {
		float effective_frame_time = 1000.0f / display_hz * (float)m->frame_repeat_count;
		snprintf(line, sizeof(line), "  Frame time:");
		tray_render_label(&mod, line, padding, y_offset + line_height, label_color);
		snprintf(line, sizeof(line), "%.2f ms", effective_frame_time);
		tray_render_label(&mod, line, col2_x, y_offset + line_height, smooth_color);
		y_offset += line_height;
	}

	/* Judder score */
	snprintf(line, sizeof(line), "  Judder:");
	tray_render_label(&mod, line, padding, y_offset + line_height, label_color);
	if (m->estimated_game_fps > 0.0f) {
		if (m->game_vrr_active || (m->frame_repeat_enabled && m->judder_score < 5)) {
			snprintf(line, sizeof(line), "None");
			tray_render_label(&mod, line, col2_x, y_offset + line_height, good_color);
		} else if (m->judder_score < 20) {
			snprintf(line, sizeof(line), "Low (%d%%)", m->judder_score);
			tray_render_label(&mod, line, col2_x, y_offset + line_height, good_color);
		} else if (m->judder_score < 50) {
			snprintf(line, sizeof(line), "Medium (%d%%)", m->judder_score);
			tray_render_label(&mod, line, col2_x, y_offset + line_height, warn_color);
		} else {
			snprintf(line, sizeof(line), "High (%d%%)", m->judder_score);
			tray_render_label(&mod, line, col2_x, y_offset + line_height, bad_color);
		}
	} else {
		snprintf(line, sizeof(line), "--");
		tray_render_label(&mod, line, col2_x, y_offset + line_height, disabled_color);
	}
	y_offset += line_height;

	/* Frames repeated counter */
	if (m->frames_repeated > 0) {
		snprintf(line, sizeof(line), "%lu", m->frames_repeated);
		y_offset += stats_render_field(&mod, "Repeated:", line,
				y_offset, line_height, padding, col2_x, label_color, smooth_color);
	}
	y_offset += 8;

	/* Separator */
	y_offset += stats_render_separator(m->stats_panel_tree, y_offset,
			m->stats_panel_width, padding, sep_color);

	/* ============ REAL-TIME COMPARISON SECTION ============ */
	y_offset += stats_render_section_header(&mod, "Real-Time Sync",
			y_offset, line_height, padding, section_color);

	/* Two-column headers */
	snprintf(line, sizeof(line), "  SCREEN");
	tray_render_label(&mod, line, padding, y_offset + line_height, label_color);
	snprintf(line, sizeof(line), "GAME");
	tray_render_label(&mod, line, col2_x, y_offset + line_height, label_color);
	y_offset += line_height;

	/* Refresh rates - big numbers */
	snprintf(line, sizeof(line), "  %.1f Hz", display_hz);
	tray_render_label(&mod, line, padding, y_offset + line_height,
		is_synced ? sync_color : value_color);

	if (m->estimated_game_fps > 0.0f) {
		snprintf(line, sizeof(line), "%.1f FPS", m->estimated_game_fps);
		tray_render_label(&mod, line, col2_x, y_offset + line_height,
			is_synced ? sync_color :
			(m->estimated_game_fps >= 55.0f ? good_color :
			(m->estimated_game_fps >= 30.0f ? warn_color : bad_color)));
	} else {
		snprintf(line, sizeof(line), "-- FPS");
		tray_render_label(&mod, line, col2_x, y_offset + line_height, disabled_color);
	}
	y_offset += line_height;

	/* Sync status indicator */
	if (m->estimated_game_fps > 0.0f) {
		if (is_synced) {
			snprintf(line, sizeof(line), "  [SYNCED]");
			tray_render_label(&mod, line, padding, y_offset + line_height, sync_color);
		} else {
			snprintf(line, sizeof(line), "  Diff: %.1f Hz", fps_diff);
			tray_render_label(&mod, line, padding, y_offset + line_height,
				fps_diff < 5.0f ? warn_color : bad_color);
		}
		y_offset += line_height;
	}
	y_offset += 8;

	/* Separator */
	y_offset += stats_render_separator(m->stats_panel_tree, y_offset,
			m->stats_panel_width, padding, sep_color);

	/* ============ RENDERING SECTION ============ */
	y_offset += stats_render_section_header(&mod, "Rendering",
			y_offset, line_height, padding, section_color);

	snprintf(line, sizeof(line), "  Mode:");
	tray_render_label(&mod, line, padding, y_offset + line_height, label_color);
	if (m->direct_scanout_active) {
		snprintf(line, sizeof(line), "Direct Scanout");
		tray_render_label(&mod, line, col2_x, y_offset + line_height, good_color);
	} else {
		snprintf(line, sizeof(line), "Composited");
		tray_render_label(&mod, line, col2_x, y_offset + line_height, value_color);
	}
	y_offset += line_height;

	snprintf(line, sizeof(line), "  Commit:");
	tray_render_label(&mod, line, padding, y_offset + line_height, label_color);
	snprintf(line, sizeof(line), "%.2f ms", commit_ms);
	tray_render_label(&mod, line, col2_x, y_offset + line_height,
		commit_ms < 2.0f ? good_color : (commit_ms < 8.0f ? warn_color : bad_color));
	y_offset += line_height;

	if (avg_latency_ms > 0.0f) {
		snprintf(line, sizeof(line), "  Latency:");
		tray_render_label(&mod, line, padding, y_offset + line_height, label_color);
		snprintf(line, sizeof(line), "%.1f ms", avg_latency_ms);
		tray_render_label(&mod, line, col2_x, y_offset + line_height,
			avg_latency_ms < 16.0f ? good_color :
			(avg_latency_ms < 33.0f ? warn_color : bad_color));
		y_offset += line_height;
	}
	y_offset += 8;

	/* Separator */
	y_offset += stats_render_separator(m->stats_panel_tree, y_offset,
			m->stats_panel_width, padding, sep_color);

	/* ============ GAME MODE SECTION ============ */
	y_offset += stats_render_section_header(&mod, "Game Mode",
			y_offset, line_height, padding, section_color);

	snprintf(line, sizeof(line), "  Status:");
	tray_render_label(&mod, line, padding, y_offset + line_height, label_color);
	if (game_mode_ultra) {
		static const float ultra_color[4] = {1.0f, 0.2f, 0.8f, 1.0f};  /* Hot pink for ULTRA */
		snprintf(line, sizeof(line), "ULTRA");
		tray_render_label(&mod, line, col2_x, y_offset + line_height, ultra_color);
	} else if (game_mode_active) {
		snprintf(line, sizeof(line), "Active");
		tray_render_label(&mod, line, col2_x, y_offset + line_height, good_color);
	} else {
		snprintf(line, sizeof(line), "Inactive");
		tray_render_label(&mod, line, col2_x, y_offset + line_height, disabled_color);
	}
	y_offset += line_height;

	/* Show what's optimized in game mode */
	snprintf(line, sizeof(line), "  Timers:");
	tray_render_label(&mod, line, padding, y_offset + line_height, label_color);
	if (game_mode_ultra) {
		snprintf(line, sizeof(line), "All Stopped");
		tray_render_label(&mod, line, col2_x, y_offset + line_height, good_color);
	} else if (game_mode_active) {
		snprintf(line, sizeof(line), "Some Paused");
		tray_render_label(&mod, line, col2_x, y_offset + line_height, warn_color);
	} else {
		snprintf(line, sizeof(line), "Normal");
		tray_render_label(&mod, line, col2_x, y_offset + line_height, value_color);
	}
	y_offset += line_height;

	snprintf(line, sizeof(line), "  Statusbar:");
	tray_render_label(&mod, line, padding, y_offset + line_height, label_color);
	if (game_mode_ultra) {
		snprintf(line, sizeof(line), "Hidden");
		tray_render_label(&mod, line, col2_x, y_offset + line_height, good_color);
	} else {
		snprintf(line, sizeof(line), "Visible");
		tray_render_label(&mod, line, col2_x, y_offset + line_height, value_color);
	}
	y_offset += line_height;

	/* Show game client info if available */
	if (game_mode_client) {
		const char *app_id = client_get_appid(game_mode_client);
		if (app_id && strlen(app_id) > 0) {
			snprintf(line, sizeof(line), "  Game:");
			tray_render_label(&mod, line, padding, y_offset + line_height, label_color);
			/* Truncate app_id if too long */
			char truncated[32];
			strncpy(truncated, app_id, sizeof(truncated) - 1);
			truncated[sizeof(truncated) - 1] = '\0';
			if (strlen(app_id) > 24) {
				truncated[21] = '.';
				truncated[22] = '.';
				truncated[23] = '.';
				truncated[24] = '\0';
			}
			tray_render_label(&mod, truncated, col2_x, y_offset + line_height, value_color);
			y_offset += line_height;
		}
	}

	/* Show priority boost if active */
	if (game_mode_nice_applied || game_mode_ioclass_applied) {
		y_offset += stats_render_field(&mod, "Priority:", "Boosted",
				y_offset, line_height, padding, col2_x, label_color, good_color);
	}
	y_offset += 8;

	/* Separator */
	y_offset += stats_render_separator(m->stats_panel_tree, y_offset,
			m->stats_panel_width, padding, sep_color);

	/* ============ INPUT LATENCY SECTION ============ */
	if (game_mode_ultra) {
		static const float latency_color[4] = {1.0f, 0.6f, 0.2f, 1.0f};  /* Orange */
		y_offset += stats_render_section_header(&mod, "Input Latency",
				y_offset, line_height, padding, section_color);

		/* Current input-to-frame latency */
		snprintf(line, sizeof(line), "  Current:");
		tray_render_label(&mod, line, padding, y_offset + line_height, label_color);
		if (m->input_to_frame_ns > 0) {
			float latency_ms = (float)m->input_to_frame_ns / 1000000.0f;
			snprintf(line, sizeof(line), "%.1f ms", latency_ms);
			tray_render_label(&mod, line, col2_x, y_offset + line_height,
				latency_ms < 8.0f ? good_color :
				(latency_ms < 16.0f ? latency_color : bad_color));
		} else {
			snprintf(line, sizeof(line), "-- ms");
			tray_render_label(&mod, line, col2_x, y_offset + line_height, disabled_color);
		}
		y_offset += line_height;

		/* Min/Max latency */
		if (m->min_input_latency_ns > 0) {
			snprintf(line, sizeof(line), "%.1f-%.1f ms",
				(float)m->min_input_latency_ns / 1000000.0f,
				(float)m->max_input_latency_ns / 1000000.0f);
			y_offset += stats_render_field(&mod, "Range:", line,
					y_offset, line_height, padding, col2_x, label_color, value_color);
		}

		/* Frame timing jitter/variance */
		if (m->frame_variance_ns > 0) {
			/* Convert variance to approximate jitter in ms (sqrt approximation) */
			float jitter_ms = 0.0f;
			uint64_t v = m->frame_variance_ns;
			uint64_t sqrt_approx = 0;
			while (sqrt_approx * sqrt_approx < v) sqrt_approx += 10000;
			jitter_ms = (float)sqrt_approx / 1000000.0f;
			snprintf(line, sizeof(line), "%.2f ms", jitter_ms);
			y_offset += stats_render_field(&mod, "Jitter:", line,
					y_offset, line_height, padding, col2_x, label_color,
					jitter_ms < 1.0f ? good_color :
					(jitter_ms < 3.0f ? warn_color : bad_color));
		}

		/* Prediction accuracy */
		if (m->prediction_accuracy > 0.0f) {
			snprintf(line, sizeof(line), "%.0f%% accurate", m->prediction_accuracy);
			y_offset += stats_render_field(&mod, "Prediction:", line,
					y_offset, line_height, padding, col2_x, label_color,
					m->prediction_accuracy > 80.0f ? good_color :
					(m->prediction_accuracy > 50.0f ? warn_color : bad_color));
			y_offset += line_height;
		}
		y_offset += 8;

		y_offset += stats_render_separator(m->stats_panel_tree, y_offset,
				m->stats_panel_width, padding, sep_color);
	}

	/* ============ SYSTEM HEALTH SECTION ============ */
	{
		int mem_pressure = get_memory_pressure();
		m->memory_pressure = mem_pressure;

		y_offset += stats_render_section_header(&mod, "System Health",
				y_offset, line_height, padding, section_color);

		snprintf(line, sizeof(line), "%d%% used", mem_pressure);
		y_offset += stats_render_field(&mod, "Memory:", line,
				y_offset, line_height, padding, col2_x, label_color,
				mem_pressure < 70 ? good_color :
				(mem_pressure < 90 ? warn_color : bad_color));

		/* Show warning if memory pressure is high */
		if (mem_pressure >= 90) {
			snprintf(line, sizeof(line), "  ⚠ Low memory!");
			tray_render_label(&mod, line, padding, y_offset + line_height, bad_color);
			y_offset += line_height;
		}
		y_offset += 8;
	}

	/* Separator */
	y_offset += stats_render_separator(m->stats_panel_tree, y_offset,
			m->stats_panel_width, padding, sep_color);

	/* ============ FRAME STATS SECTION ============ */
	y_offset += stats_render_section_header(&mod, "Frame Statistics",
			y_offset, line_height, padding, section_color);

	snprintf(line, sizeof(line), "%lu", m->frames_presented);
	y_offset += stats_render_field(&mod, "Presented:", line,
			y_offset, line_height, padding, col2_x, label_color, value_color);

	snprintf(line, sizeof(line), "%lu", m->frames_dropped);
	y_offset += stats_render_field(&mod, "Dropped:", line,
			y_offset, line_height, padding, col2_x, label_color,
			m->frames_dropped == 0 ? good_color : bad_color);

	snprintf(line, sizeof(line), "%lu", m->frames_held);
	y_offset += stats_render_field(&mod, "Held:", line,
			y_offset, line_height, padding, col2_x, label_color,
			m->frames_held < 10 ? good_color : warn_color);
	y_offset += 8;

	/* Separator */
	y_offset += stats_render_separator(m->stats_panel_tree, y_offset,
			m->stats_panel_width, padding, sep_color);

	/* ============ FPS LIMITER SECTION ============ */
	y_offset += stats_render_section_header(&mod, "FPS Limiter",
			y_offset, line_height, padding, section_color);

	snprintf(line, sizeof(line), "  Status:");
	tray_render_label(&mod, line, padding, y_offset + line_height, label_color);
	if (fps_limit_enabled) {
		snprintf(line, sizeof(line), "ON (%d FPS)", fps_limit_value);
		tray_render_label(&mod, line, col2_x, y_offset + line_height, warn_color);
	} else {
		snprintf(line, sizeof(line), "OFF");
		tray_render_label(&mod, line, col2_x, y_offset + line_height, value_color);
	}
	y_offset += line_height;

	/* Show effective limit vs actual */
	if (fps_limit_enabled && m->estimated_game_fps > 0.0f) {
		snprintf(line, sizeof(line), "  Effective:");
		tray_render_label(&mod, line, padding, y_offset + line_height, label_color);
		float effective = m->estimated_game_fps;
		if (effective > (float)fps_limit_value)
			effective = (float)fps_limit_value;
		snprintf(line, sizeof(line), "%.0f FPS", effective);
		tray_render_label(&mod, line, col2_x, y_offset + line_height,
			effective >= (float)fps_limit_value - 1.0f ? good_color : warn_color);
		y_offset += line_height;
	}
	y_offset += 8;

	/* Separator */
	y_offset += stats_render_separator(m->stats_panel_tree, y_offset,
			m->stats_panel_width, padding, sep_color);

	/* ============ CONTROLS SECTION ============ */
	y_offset += stats_render_section_header(&mod, "Controls",
			y_offset, line_height, padding, section_color);

	static const float hint_color[4] = {0.5f, 0.5f, 0.6f, 1.0f};
	static const float key_color[4] = {0.7f, 0.8f, 0.9f, 1.0f};

	snprintf(line, sizeof(line), "  [L]");
	tray_render_label(&mod, line, padding, y_offset + line_height, key_color);
	snprintf(line, sizeof(line), "Toggle FPS limit");
	tray_render_label(&mod, line, padding + 50, y_offset + line_height, hint_color);
	y_offset += line_height;

	snprintf(line, sizeof(line), "  [V]");
	tray_render_label(&mod, line, padding, y_offset + line_height, key_color);
	snprintf(line, sizeof(line), "Toggle VRR");
	tray_render_label(&mod, line, padding + 50, y_offset + line_height, hint_color);
	y_offset += line_height;

	snprintf(line, sizeof(line), "  [+/-]");
	tray_render_label(&mod, line, padding, y_offset + line_height, key_color);
	snprintf(line, sizeof(line), "Adjust +/- 10");
	tray_render_label(&mod, line, padding + 50, y_offset + line_height, hint_color);
	y_offset += line_height;

	snprintf(line, sizeof(line), "  [1-5]");
	tray_render_label(&mod, line, padding, y_offset + line_height, key_color);
	snprintf(line, sizeof(line), "30/60/90/120/144");
	tray_render_label(&mod, line, padding + 50, y_offset + line_height, hint_color);
	y_offset += line_height;

	snprintf(line, sizeof(line), "  [0]");
	tray_render_label(&mod, line, padding, y_offset + line_height, key_color);
	snprintf(line, sizeof(line), "Disable limit");
	tray_render_label(&mod, line, padding + 50, y_offset + line_height, hint_color);
	y_offset += line_height;

	snprintf(line, sizeof(line), "  [Esc]");
	tray_render_label(&mod, line, padding, y_offset + line_height, key_color);
	snprintf(line, sizeof(line), "Close panel");
	tray_render_label(&mod, line, padding + 50, y_offset + line_height, hint_color);
	y_offset += line_height;

	/* Schedule next refresh (100ms for smoother real-time updates) */
	wl_event_source_timer_update(m->stats_panel_timer, 100);

	return 0;
}

void
gamepanel(const Arg *arg)
{
	Monitor *m = selmon;

	wlr_log(WLR_INFO, "gamepanel() called, selmon=%p", (void*)m);

	if (!m)
		return;

	/* Calculate panel width (25% of screen) */
	m->stats_panel_width = m->m.width / 4;
	if (m->stats_panel_width < 200)
		m->stats_panel_width = 200;

	/* Create panel tree if needed - use LyrBlock to show over fullscreen */
	if (!m->stats_panel_tree) {
		m->stats_panel_tree = wlr_scene_tree_create(layers[LyrBlock]);
		if (!m->stats_panel_tree)
			return;
		m->stats_panel_visible = 0;
		m->stats_panel_animating = 0;
		/* Start off-screen */
		m->stats_panel_current_x = m->m.x + m->m.width;
	}

	/* Create timers if needed */
	if (!m->stats_panel_timer)
		m->stats_panel_timer = wl_event_loop_add_timer(event_loop,
			stats_panel_refresh_cb, m);
	if (!m->stats_panel_anim_timer)
		m->stats_panel_anim_timer = wl_event_loop_add_timer(event_loop,
			stats_panel_anim_cb, m);

	/* Toggle visibility */
	if (m->stats_panel_visible) {
		wlr_log(WLR_INFO, "gamepanel: sliding OUT, panel was visible");
		/* Slide out */
		m->stats_panel_target_x = m->m.x + m->m.width;
		m->stats_panel_anim_start = get_time_ns();
		m->stats_panel_animating = 1;
		/* Start animation from current position */
		wl_event_source_timer_update(m->stats_panel_anim_timer, 1);
		/* Cancel refresh timer */
		wl_event_source_timer_update(m->stats_panel_timer, 0);
	} else {
		wlr_log(WLR_INFO, "gamepanel: sliding IN, panel_tree=%p, width=%d, pos=(%d,%d)",
			(void*)m->stats_panel_tree, m->stats_panel_width,
			m->stats_panel_target_x, m->m.y);
		/* Slide in */
		m->stats_panel_visible = 1;
		m->stats_panel_target_x = m->m.x + m->m.width - m->stats_panel_width;
		m->stats_panel_current_x = m->m.x + m->m.width; /* Start off-screen */
		m->stats_panel_anim_start = get_time_ns();
		m->stats_panel_animating = 1;

		/* Enable and position */
		wlr_scene_node_set_enabled(&m->stats_panel_tree->node, 1);
		wlr_scene_node_set_position(&m->stats_panel_tree->node,
			m->stats_panel_current_x, m->m.y);
		/* Ensure panel is on top of other overlays in LyrBlock */
		wlr_scene_node_raise_to_top(&m->stats_panel_tree->node);

		wlr_log(WLR_INFO, "gamepanel: enabled node at x=%d y=%d, raised to top, starting animation",
			m->stats_panel_current_x, m->m.y);

		/* Initial render and start animation */
		stats_panel_refresh_cb(m);
		wl_event_source_timer_update(m->stats_panel_anim_timer, 1);
	}
}

Monitor *
stats_panel_visible_monitor(void)
{
	Monitor *m;

	wl_list_for_each(m, &mons, link) {
		if (m->stats_panel_visible)
			return m;
	}
	return NULL;
}

int
stats_panel_handle_key(Monitor *m, xkb_keysym_t sym)
{
	char osd_msg[64];

	if (!m || !m->stats_panel_visible)
		return 0;

	switch (sym) {
	case XKB_KEY_l:
	case XKB_KEY_L:
		/* Toggle FPS limiter */
		fps_limit_enabled = !fps_limit_enabled;
		if (fps_limit_enabled) {
			snprintf(osd_msg, sizeof(osd_msg), "FPS Limit: %d", fps_limit_value);
		} else {
			snprintf(osd_msg, sizeof(osd_msg), "FPS Limit: OFF");
		}
		show_hz_osd(m, osd_msg);
		stats_panel_refresh_cb(m);
		return 1;

	case XKB_KEY_plus:
	case XKB_KEY_equal:
	case XKB_KEY_KP_Add:
		/* Increase FPS limit by 10 */
		fps_limit_value += 10;
		if (fps_limit_value > 500)
			fps_limit_value = 500;
		if (fps_limit_enabled) {
			snprintf(osd_msg, sizeof(osd_msg), "FPS Limit: %d", fps_limit_value);
			show_hz_osd(m, osd_msg);
		}
		stats_panel_refresh_cb(m);
		return 1;

	case XKB_KEY_minus:
	case XKB_KEY_KP_Subtract:
		/* Decrease FPS limit by 10 */
		fps_limit_value -= 10;
		if (fps_limit_value < 10)
			fps_limit_value = 10;
		if (fps_limit_enabled) {
			snprintf(osd_msg, sizeof(osd_msg), "FPS Limit: %d", fps_limit_value);
			show_hz_osd(m, osd_msg);
		}
		stats_panel_refresh_cb(m);
		return 1;

	case XKB_KEY_1:
		fps_limit_value = 30;
		fps_limit_enabled = 1;
		snprintf(osd_msg, sizeof(osd_msg), "FPS Limit: 30");
		show_hz_osd(m, osd_msg);
		stats_panel_refresh_cb(m);
		return 1;

	case XKB_KEY_2:
		fps_limit_value = 60;
		fps_limit_enabled = 1;
		snprintf(osd_msg, sizeof(osd_msg), "FPS Limit: 60");
		show_hz_osd(m, osd_msg);
		stats_panel_refresh_cb(m);
		return 1;

	case XKB_KEY_3:
		fps_limit_value = 90;
		fps_limit_enabled = 1;
		snprintf(osd_msg, sizeof(osd_msg), "FPS Limit: 90");
		show_hz_osd(m, osd_msg);
		stats_panel_refresh_cb(m);
		return 1;

	case XKB_KEY_4:
		fps_limit_value = 120;
		fps_limit_enabled = 1;
		snprintf(osd_msg, sizeof(osd_msg), "FPS Limit: 120");
		show_hz_osd(m, osd_msg);
		stats_panel_refresh_cb(m);
		return 1;

	case XKB_KEY_5:
		fps_limit_value = 144;
		fps_limit_enabled = 1;
		snprintf(osd_msg, sizeof(osd_msg), "FPS Limit: 144");
		show_hz_osd(m, osd_msg);
		stats_panel_refresh_cb(m);
		return 1;

	case XKB_KEY_0:
		/* Disable limiter */
		fps_limit_enabled = 0;
		show_hz_osd(m, "FPS Limit: OFF");
		stats_panel_refresh_cb(m);
		return 1;

	case XKB_KEY_v:
	case XKB_KEY_V:
		/* Toggle VRR */
		fullscreen_adaptive_sync_enabled = !fullscreen_adaptive_sync_enabled;
		if (fullscreen_adaptive_sync_enabled) {
			show_hz_osd(m, "VRR: ENABLED");
		} else {
			show_hz_osd(m, "VRR: DISABLED");
			/* Disable active VRR if it was on */
			if (m->game_vrr_active)
				disable_game_vrr(m);
		}
		stats_panel_refresh_cb(m);
		return 1;

	case XKB_KEY_Escape:
		/* Close panel */
		gamepanel(NULL);
		return 1;
	}

	return 0;
}


void htpc_menu_build(void)
{
	htpc_menu_item_count = 0;

	/* Order matches tag numbers: TV-shows=1, Movies=2, Retro-gaming=3, PC-gaming=4 */
	if (htpc_page_tvshows) {
		snprintf(htpc_menu_items[htpc_menu_item_count].label, 64, "TV-shows");
		htpc_menu_items[htpc_menu_item_count].command[0] = '\0';
		htpc_menu_item_count++;
	}
	if (htpc_page_movies) {
		snprintf(htpc_menu_items[htpc_menu_item_count].label, 64, "Movies");
		htpc_menu_items[htpc_menu_item_count].command[0] = '\0';
		htpc_menu_item_count++;
	}
	if (htpc_page_retrogaming) {
		snprintf(htpc_menu_items[htpc_menu_item_count].label, 64, "Retro-gaming");
		htpc_menu_items[htpc_menu_item_count].command[0] = '\0';
		htpc_menu_item_count++;
	}
	if (htpc_page_pcgaming) {
		snprintf(htpc_menu_items[htpc_menu_item_count].label, 64, "PC-gaming");
		htpc_menu_items[htpc_menu_item_count].command[0] = '\0';
		htpc_menu_item_count++;
	}
	/* Live TV streaming services */
	if (htpc_page_nrk) {
		snprintf(htpc_menu_items[htpc_menu_item_count].label, 64, "NRK");
		snprintf(htpc_menu_items[htpc_menu_item_count].command, 256, "%s", NRK_URL);
		htpc_menu_item_count++;
	}
	if (htpc_page_netflix) {
		snprintf(htpc_menu_items[htpc_menu_item_count].label, 64, "Netflix");
		snprintf(htpc_menu_items[htpc_menu_item_count].command, 256, "%s", NETFLIX_URL);
		htpc_menu_item_count++;
	}
	if (htpc_page_viaplay) {
		snprintf(htpc_menu_items[htpc_menu_item_count].label, 64, "Viaplay");
		snprintf(htpc_menu_items[htpc_menu_item_count].command, 256, "%s", VIAPLAY_URL);
		htpc_menu_item_count++;
	}
	if (htpc_page_tv2play) {
		snprintf(htpc_menu_items[htpc_menu_item_count].label, 64, "TV2 Play");
		snprintf(htpc_menu_items[htpc_menu_item_count].command, 256, "%s", TV2PLAY_URL);
		htpc_menu_item_count++;
	}
	if (htpc_page_f1tv) {
		snprintf(htpc_menu_items[htpc_menu_item_count].label, 64, "F1TV");
		snprintf(htpc_menu_items[htpc_menu_item_count].command, 256, "%s", F1TV_URL);
		htpc_menu_item_count++;
	}
}
