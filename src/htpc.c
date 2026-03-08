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

/*
 * Check if a PID is a child (direct or indirect) of a given parent PID.
 * Walks the ppid chain up to avoid freezing game/compositor children.
 */
static int
is_child_of(pid_t pid, pid_t parent)
{
	char path[64];
	FILE *fp;
	char line[256];
	pid_t ppid;
	int depth = 0;

	if (pid == parent)
		return 1;

	ppid = pid;
	while (ppid > 1 && depth < 32) {
		snprintf(path, sizeof(path), "/proc/%d/stat", ppid);
		fp = fopen(path, "r");
		if (!fp)
			return 0;
		/* stat format: pid (comm) state ppid ... */
		if (!fgets(line, sizeof(line), fp)) {
			fclose(fp);
			return 0;
		}
		fclose(fp);
		/* Find the closing ')' to skip comm field (may contain spaces) */
		char *cp = strrchr(line, ')');
		if (!cp)
			return 0;
		/* After ') ' comes state, then ppid */
		int scanned = sscanf(cp + 2, "%*c %d", &ppid);
		if (scanned != 1)
			return 0;
		if (ppid == parent)
			return 1;
		depth++;
	}
	return 0;
}

/*
 * Freeze background processes during game mode.
 * Sends SIGSTOP to all user-owned processes except whitelisted ones.
 * Safe to call multiple times - clears state on each call.
 */
void
freeze_background_processes(void)
{
	DIR *dir;
	struct dirent *ent;
	char path[128];
	char comm[64];
	char uid_line[256];
	FILE *fp;
	uid_t our_uid = getuid();
	pid_t our_pid = getpid();
	pid_t pid;
	int i;

	/* Whitelist of process comm names that should never be frozen */
	static const char *whitelist[] = {
		"nixlytile", "Xwayland", "xwayland",
		"pipewire", "wireplumber", "pulseaudio", "pipewire-pulse",
		"swaybg", "dbus-daemon", "dbus-broker",
		"steam", "steamwebhelper", "reaper", "pressure-vessel",
		"discord", "Discord", "vesktop", "Vesktop", "webcord", "WebCord",
		"armcord", "ArmCord", "legcord", "Legcord",
		"ssh", "sshd", "gpg-agent",
		NULL
	};

	/* Reset state for idempotent re-entry */
	frozen_pid_count = 0;

	dir = opendir("/proc");
	if (!dir)
		return;

	while ((ent = readdir(dir))) {
		/* Only look at numeric (PID) entries */
		if (ent->d_name[0] < '0' || ent->d_name[0] > '9')
			continue;

		pid = (pid_t)atoi(ent->d_name);
		if (pid <= 1 || pid == our_pid)
			continue;

		/* Check if this process belongs to our user */
		snprintf(path, sizeof(path), "/proc/%d/status", pid);
		fp = fopen(path, "r");
		if (!fp)
			continue;

		int is_our_uid = 0;
		while (fgets(uid_line, sizeof(uid_line), fp)) {
			if (strncmp(uid_line, "Uid:", 4) == 0) {
				uid_t real_uid;
				if (sscanf(uid_line + 4, "%u", &real_uid) == 1 && real_uid == our_uid)
					is_our_uid = 1;
				break;
			}
		}
		fclose(fp);

		if (!is_our_uid)
			continue;

		/* Read the process comm name */
		snprintf(path, sizeof(path), "/proc/%d/comm", pid);
		fp = fopen(path, "r");
		if (!fp)
			continue;
		comm[0] = '\0';
		if (fgets(comm, sizeof(comm), fp)) {
			char *nl = strchr(comm, '\n');
			if (nl) *nl = '\0';
		}
		fclose(fp);

		/* Check whitelist */
		int whitelisted = 0;
		for (i = 0; whitelist[i]; i++) {
			if (strcmp(comm, whitelist[i]) == 0) {
				whitelisted = 1;
				break;
			}
		}
		if (whitelisted)
			continue;

		/* Don't freeze the game process or its children */
		if (game_mode_pid > 1 && is_child_of(pid, game_mode_pid))
			continue;

		/* Don't freeze compositor children */
		if (is_child_of(pid, our_pid))
			continue;

		/* Freeze this process */
		if (frozen_pid_count < 4096) {
			if (kill(pid, SIGSTOP) == 0) {
				frozen_pids[frozen_pid_count++] = pid;
			}
		}
	}

	closedir(dir);
	wlr_log(WLR_INFO, "Froze %d background processes for game mode", frozen_pid_count);
}

/*
 * Unfreeze all previously frozen processes.
 * Sends SIGCONT to each stored PID. Harmless if PID has already exited.
 */
void
unfreeze_background_processes(void)
{
	int i;

	if (frozen_pid_count == 0)
		return;

	for (i = 0; i < frozen_pid_count; i++) {
		kill(frozen_pids[i], SIGCONT);  /* ESRCH if exited - harmless */
	}

	wlr_log(WLR_INFO, "Unfroze %d background processes", frozen_pid_count);
	frozen_pid_count = 0;
}

/*
 * Apply memory optimizations for game mode.
 * Drops page cache, lowers swappiness, and triggers memory compaction.
 */
void
apply_memory_optimization(void)
{
	int fd;

	/* Sync and drop page cache */
	sync();
	fd = open("/proc/sys/vm/drop_caches", O_WRONLY);
	if (fd >= 0) {
		write(fd, "3\n", 2);
		close(fd);
	}

	/* Lower swappiness to minimize swap thrashing during gaming */
	fd = open("/proc/sys/vm/swappiness", O_WRONLY);
	if (fd >= 0) {
		write(fd, "10\n", 3);
		close(fd);
		game_mode_swappiness_applied = 1;
	}

	/* Trigger memory compaction for huge pages */
	fd = open("/proc/sys/vm/compact_memory", O_WRONLY);
	if (fd >= 0) {
		write(fd, "1\n", 2);
		close(fd);
	}

	wlr_log(WLR_INFO, "Memory optimization applied (drop_caches, swappiness=10, compact)");
}

/*
 * Restore memory settings to defaults after game mode exits.
 */
void
restore_memory_optimization(void)
{
	int fd;

	if (game_mode_swappiness_applied) {
		fd = open("/proc/sys/vm/swappiness", O_WRONLY);
		if (fd >= 0) {
			write(fd, "60\n", 3);
			close(fd);
		}
		game_mode_swappiness_applied = 0;
		wlr_log(WLR_INFO, "Memory optimization restored (swappiness=60)");
	}
}

/*
 * CPU DMA Latency (PM QoS) — Prevent deep C-states.
 * Writing 0 to /dev/cpu_dma_latency and keeping the fd open prevents
 * the CPU from entering deep sleep states (C3+), eliminating wakeup
 * latency spikes that cause micro-stuttering.
 */
static int cpu_dma_latency_fd = -1;

void
apply_cpu_latency_qos(void)
{
	int32_t latency = 0;
	cpu_dma_latency_fd = open("/dev/cpu_dma_latency", O_WRONLY);
	if (cpu_dma_latency_fd >= 0) {
		write(cpu_dma_latency_fd, &latency, sizeof(latency));
		/* FD must stay open to maintain the QoS constraint */
		wlr_log(WLR_INFO, "CPU DMA latency QoS: set to 0 (prevent deep C-states)");
	} else {
		wlr_log(WLR_INFO, "CPU DMA latency QoS: open failed: %s", strerror(errno));
	}
}

void
restore_cpu_latency_qos(void)
{
	if (cpu_dma_latency_fd >= 0) {
		close(cpu_dma_latency_fd);  /* Closing releases the QoS constraint */
		cpu_dma_latency_fd = -1;
		wlr_log(WLR_INFO, "CPU DMA latency QoS: restored (fd closed)");
	}
}

/*
 * CPU Affinity — Core isolation.
 * Pin the compositor to core 0 and the game to cores 1..N-1 to prevent
 * them from competing for the same CPU cache lines and scheduler slots.
 */
void
apply_cpu_affinity(pid_t game_pid)
{
	int ncores = sysconf(_SC_NPROCESSORS_ONLN);
	if (ncores < 4 || game_pid <= 1) return;

	/* Pin compositor to core 0 */
	cpu_set_t compositor_set;
	CPU_ZERO(&compositor_set);
	CPU_SET(0, &compositor_set);
	sched_setaffinity(0, sizeof(compositor_set), &compositor_set);

	/* Pin game to cores 1..N-1 (all except core 0) */
	cpu_set_t game_set;
	CPU_ZERO(&game_set);
	for (int i = 1; i < ncores; i++)
		CPU_SET(i, &game_set);
	sched_setaffinity(game_pid, sizeof(game_set), &game_set);

	game_mode_affinity_applied = 1;
	wlr_log(WLR_INFO, "CPU affinity: compositor→core0, game PID %d→cores 1-%d",
		game_pid, ncores - 1);
}

void
restore_cpu_affinity(pid_t game_pid)
{
	if (!game_mode_affinity_applied) return;
	int ncores = sysconf(_SC_NPROCESSORS_ONLN);

	/* Restore both to all cores */
	cpu_set_t all_set;
	CPU_ZERO(&all_set);
	for (int i = 0; i < ncores; i++)
		CPU_SET(i, &all_set);
	sched_setaffinity(0, sizeof(all_set), &all_set);
	if (game_pid > 1)
		sched_setaffinity(game_pid, sizeof(all_set), &all_set);

	game_mode_affinity_applied = 0;
	wlr_log(WLR_INFO, "CPU affinity: restored to all cores");
}

/*
 * Transparent Huge Pages (THP).
 * Enabling THP reduces TLB misses for games with large memory allocations.
 */
static char thp_saved_value[32] = "";

void
apply_transparent_hugepages(void)
{
	int fd;
	char buf[64];
	ssize_t n;

	/* Save current setting */
	fd = open("/sys/kernel/mm/transparent_hugepage/enabled", O_RDONLY);
	if (fd >= 0) {
		n = read(fd, buf, sizeof(buf) - 1);
		close(fd);
		if (n > 0) {
			buf[n] = '\0';
			/* Extract current value from [brackets] */
			char *start = strchr(buf, '[');
			char *end = start ? strchr(start, ']') : NULL;
			if (start && end) {
				int len = end - start - 1;
				if (len < (int)sizeof(thp_saved_value)) {
					memcpy(thp_saved_value, start + 1, len);
					thp_saved_value[len] = '\0';
				}
			}
		}
	}

	/* Set to "always" for maximum hugepage usage */
	fd = open("/sys/kernel/mm/transparent_hugepage/enabled", O_WRONLY);
	if (fd >= 0) {
		write(fd, "always\n", 7);
		close(fd);
		wlr_log(WLR_INFO, "THP: set to 'always' (was '%s')", thp_saved_value);
	} else {
		wlr_log(WLR_INFO, "THP: open failed: %s", strerror(errno));
	}
}

void
restore_transparent_hugepages(void)
{
	int fd;
	if (thp_saved_value[0]) {
		fd = open("/sys/kernel/mm/transparent_hugepage/enabled", O_WRONLY);
		if (fd >= 0) {
			write(fd, thp_saved_value, strlen(thp_saved_value));
			close(fd);
			wlr_log(WLR_INFO, "THP: restored to '%s'", thp_saved_value);
		}
		thp_saved_value[0] = '\0';
	}
}

/*
 * I/O Scheduler — Low-latency disk access.
 * Switching NVMe/SSD to 'none' or 'mq-deadline' reduces disk I/O latency
 * during texture streaming and asset loading.
 */
#define MAX_BLOCK_DEVS 16

static struct {
	char path[128];
	char saved_scheduler[32];
} saved_io_schedulers[MAX_BLOCK_DEVS];
static int saved_io_scheduler_count = 0;

void
apply_io_scheduler(void)
{
	DIR *dir;
	struct dirent *ent;
	char path[256], buf[128];
	int fd;
	ssize_t n;

	saved_io_scheduler_count = 0;
	dir = opendir("/sys/block");
	if (!dir) return;

	while ((ent = readdir(dir)) && saved_io_scheduler_count < MAX_BLOCK_DEVS) {
		if (ent->d_name[0] == '.') continue;
		/* Skip loop/ram devices */
		if (strncmp(ent->d_name, "loop", 4) == 0 || strncmp(ent->d_name, "ram", 3) == 0)
			continue;

		snprintf(path, sizeof(path), "/sys/block/%s/queue/scheduler", ent->d_name);

		/* Read current scheduler */
		fd = open(path, O_RDONLY);
		if (fd < 0) continue;
		n = read(fd, buf, sizeof(buf) - 1);
		close(fd);
		if (n <= 0) continue;
		buf[n] = '\0';

		/* Extract active scheduler from [brackets] */
		char *start = strchr(buf, '[');
		char *end = start ? strchr(start, ']') : NULL;
		if (!start || !end) continue;

		int idx = saved_io_scheduler_count;
		int len = end - start - 1;
		if (len >= (int)sizeof(saved_io_schedulers[idx].saved_scheduler)) continue;

		strncpy(saved_io_schedulers[idx].path, path, sizeof(saved_io_schedulers[idx].path) - 1);
		saved_io_schedulers[idx].path[sizeof(saved_io_schedulers[idx].path) - 1] = '\0';
		memcpy(saved_io_schedulers[idx].saved_scheduler, start + 1, len);
		saved_io_schedulers[idx].saved_scheduler[len] = '\0';

		/* Set to "none" for lowest latency (best for NVMe), fall back to "mq-deadline" */
		fd = open(path, O_WRONLY);
		if (fd >= 0) {
			if (write(fd, "none", 4) < 0)
				write(fd, "mq-deadline", 11);
			close(fd);
			wlr_log(WLR_INFO, "I/O scheduler: %s → none (was '%s')",
				ent->d_name, saved_io_schedulers[idx].saved_scheduler);
			saved_io_scheduler_count++;
		}
	}
	closedir(dir);
}

void
restore_io_scheduler(void)
{
	int fd;
	for (int i = 0; i < saved_io_scheduler_count; i++) {
		fd = open(saved_io_schedulers[i].path, O_WRONLY);
		if (fd >= 0) {
			write(fd, saved_io_schedulers[i].saved_scheduler,
				strlen(saved_io_schedulers[i].saved_scheduler));
			close(fd);
		}
	}
	if (saved_io_scheduler_count > 0)
		wlr_log(WLR_INFO, "I/O scheduler: restored %d devices", saved_io_scheduler_count);
	saved_io_scheduler_count = 0;
}

/*
 * Disable Kernel Watchdog — Remove NMI interrupts.
 * The kernel watchdog generates periodic NMI interrupts that can cause
 * micro-stuttering. Disabling it during gaming eliminates this jitter source.
 */
static int watchdog_was_enabled = -1;

void
apply_disable_watchdog(void)
{
	int fd;
	char buf[8];
	ssize_t n;

	/* Save current state */
	fd = open("/proc/sys/kernel/nmi_watchdog", O_RDONLY);
	if (fd >= 0) {
		n = read(fd, buf, sizeof(buf) - 1);
		close(fd);
		if (n > 0) {
			buf[n] = '\0';
			watchdog_was_enabled = atoi(buf);
		}
	}

	/* Disable NMI watchdog */
	fd = open("/proc/sys/kernel/nmi_watchdog", O_WRONLY);
	if (fd >= 0) {
		write(fd, "0\n", 2);
		close(fd);
		wlr_log(WLR_INFO, "Kernel watchdog: NMI disabled");
	}

	/* Disable software watchdog too */
	fd = open("/proc/sys/kernel/watchdog", O_WRONLY);
	if (fd >= 0) {
		write(fd, "0\n", 2);
		close(fd);
		wlr_log(WLR_INFO, "Kernel watchdog: software watchdog disabled");
	}
}

void
restore_watchdog(void)
{
	int fd;
	if (watchdog_was_enabled > 0) {
		fd = open("/proc/sys/kernel/nmi_watchdog", O_WRONLY);
		if (fd >= 0) { write(fd, "1\n", 2); close(fd); }
		fd = open("/proc/sys/kernel/watchdog", O_WRONLY);
		if (fd >= 0) { write(fd, "1\n", 2); close(fd); }
		wlr_log(WLR_INFO, "Kernel watchdog: restored (was enabled)");
	}
	watchdog_was_enabled = -1;
}

/*
 * Raw Input Mode — Disable pointer acceleration.
 * Gives 1:1 mouse movement, essential for FPS/competitive games.
 */
void
apply_raw_input(void)
{
	for (int i = 0; i < pointer_device_count; i++) {
		if (libinput_device_config_accel_is_available(pointer_devices[i])) {
			libinput_device_config_accel_set_profile(pointer_devices[i],
				LIBINPUT_CONFIG_ACCEL_PROFILE_FLAT);
		}
	}
	game_mode_raw_input_applied = 1;
	wlr_log(WLR_INFO, "Raw input: disabled pointer acceleration (%d devices)", pointer_device_count);
}

void
restore_raw_input(void)
{
	if (!game_mode_raw_input_applied) return;
	for (int i = 0; i < pointer_device_count; i++) {
		if (libinput_device_config_accel_is_available(pointer_devices[i])) {
			libinput_device_config_accel_set_profile(pointer_devices[i], accel_profile);
			libinput_device_config_accel_set_speed(pointer_devices[i], accel_speed);
		}
	}
	game_mode_raw_input_applied = 0;
	wlr_log(WLR_INFO, "Raw input: restored pointer acceleration profile");
}

/*
 * IRQ Affinity — Move hardware interrupts to compositor core.
 * Moving IRQs to core 0 means game cores are completely interrupt-free.
 * This is the natural complement to CPU affinity (core isolation).
 */
#define MAX_IRQS 512

static struct {
	int irq;
	char saved_affinity[32];
} saved_irq_affinities[MAX_IRQS];
static int saved_irq_affinity_count = 0;

void
apply_irq_affinity(void)
{
	DIR *dir;
	struct dirent *ent;
	char path[256], buf[64];
	int fd;
	ssize_t n;

	saved_irq_affinity_count = 0;
	dir = opendir("/proc/irq");
	if (!dir) return;

	while ((ent = readdir(dir)) && saved_irq_affinity_count < MAX_IRQS) {
		/* Only numeric entries (IRQ numbers) */
		if (ent->d_name[0] < '0' || ent->d_name[0] > '9')
			continue;

		snprintf(path, sizeof(path), "/proc/irq/%s/smp_affinity_list", ent->d_name);

		/* Read current affinity */
		fd = open(path, O_RDONLY);
		if (fd < 0) continue;
		n = read(fd, buf, sizeof(buf) - 1);
		close(fd);
		if (n <= 0) continue;
		buf[n] = '\0';
		/* Strip trailing newline */
		if (n > 0 && buf[n - 1] == '\n') buf[n - 1] = '\0';

		int idx = saved_irq_affinity_count;
		saved_irq_affinities[idx].irq = atoi(ent->d_name);
		strncpy(saved_irq_affinities[idx].saved_affinity, buf,
			sizeof(saved_irq_affinities[idx].saved_affinity) - 1);
		saved_irq_affinities[idx].saved_affinity[
			sizeof(saved_irq_affinities[idx].saved_affinity) - 1] = '\0';

		/* Pin to core 0 only */
		fd = open(path, O_WRONLY);
		if (fd >= 0) {
			write(fd, "0", 1);
			close(fd);
			saved_irq_affinity_count++;
		}
	}
	closedir(dir);
	wlr_log(WLR_INFO, "IRQ affinity: pinned %d IRQs to core 0", saved_irq_affinity_count);
}

void
restore_irq_affinity(void)
{
	char path[256];
	int fd;

	for (int i = 0; i < saved_irq_affinity_count; i++) {
		snprintf(path, sizeof(path), "/proc/irq/%d/smp_affinity_list",
			saved_irq_affinities[i].irq);
		fd = open(path, O_WRONLY);
		if (fd >= 0) {
			write(fd, saved_irq_affinities[i].saved_affinity,
				strlen(saved_irq_affinities[i].saved_affinity));
			close(fd);
		}
	}
	if (saved_irq_affinity_count > 0)
		wlr_log(WLR_INFO, "IRQ affinity: restored %d IRQs", saved_irq_affinity_count);
	saved_irq_affinity_count = 0;
}

/*
 * CFS Scheduler Tuning — Optimize scheduler for low-latency gaming.
 * Lower granularity = faster response, higher migration cost = keep processes
 * on their assigned cores (better cache utilization with affinity).
 */
static char sched_saved_min_granularity[32] = "";
static char sched_saved_wakeup_granularity[32] = "";
static char sched_saved_migration_cost[32] = "";

static void
sched_save_and_write(const char *path, const char *value, char *save_buf, size_t save_len)
{
	int fd;
	ssize_t n;

	/* Save current value */
	fd = open(path, O_RDONLY);
	if (fd >= 0) {
		n = read(fd, save_buf, save_len - 1);
		close(fd);
		if (n > 0) {
			save_buf[n] = '\0';
			if (n > 0 && save_buf[n - 1] == '\n') save_buf[n - 1] = '\0';
		}
	}

	/* Write new value */
	fd = open(path, O_WRONLY);
	if (fd >= 0) {
		write(fd, value, strlen(value));
		close(fd);
	}
}

static void
sched_restore(const char *path, char *save_buf)
{
	int fd;
	if (save_buf[0]) {
		fd = open(path, O_WRONLY);
		if (fd >= 0) {
			write(fd, save_buf, strlen(save_buf));
			close(fd);
		}
		save_buf[0] = '\0';
	}
}

void
apply_scheduler_tuning(void)
{
	/* Lower min_granularity for faster preemption (default ~3ms → 1ms) */
	sched_save_and_write("/proc/sys/kernel/sched_min_granularity_ns",
		"1000000", sched_saved_min_granularity, sizeof(sched_saved_min_granularity));

	/* Lower wakeup_granularity for faster wakeup preemption (default ~4ms → 500μs) */
	sched_save_and_write("/proc/sys/kernel/sched_wakeup_granularity_ns",
		"500000", sched_saved_wakeup_granularity, sizeof(sched_saved_wakeup_granularity));

	/* Higher migration_cost to keep tasks on their assigned cores (default ~500μs → 5ms) */
	sched_save_and_write("/proc/sys/kernel/sched_migration_cost_ns",
		"5000000", sched_saved_migration_cost, sizeof(sched_saved_migration_cost));

	wlr_log(WLR_INFO, "Scheduler tuning: min_gran=1ms, wakeup_gran=500μs, migration_cost=5ms");
}

void
restore_scheduler_tuning(void)
{
	sched_restore("/proc/sys/kernel/sched_min_granularity_ns", sched_saved_min_granularity);
	sched_restore("/proc/sys/kernel/sched_wakeup_granularity_ns", sched_saved_wakeup_granularity);
	sched_restore("/proc/sys/kernel/sched_migration_cost_ns", sched_saved_migration_cost);
	wlr_log(WLR_INFO, "Scheduler tuning: restored defaults");
}

/*
 * GPU Power State — Force maximum GPU performance.
 * For AMD: set power_dma_perf_level to "high" and use VR power profile.
 * For Intel: set min_freq to max_freq.
 */
static char gpu_saved_perf_level[32] = "";
static char gpu_saved_power_profile[32] = "";
static char gpu_power_perf_path[128] = "";
static char gpu_power_profile_path[128] = "";
static char gpu_intel_min_path[128] = "";
static char gpu_saved_intel_min[32] = "";

/* NVIDIA state */
static int nv_clocks_locked = 0;
static int nv_persistence_was_off = 0;
static int nv_power_applied = 0;
static char nv_saved_power_limit[32] = "";
static char nv_pci_power_path[128] = "";
static char nv_saved_pci_power[32] = "";
static int nv_gpu_index = 0;  /* nvidia-smi GPU index (auto-detected) */

/*
 * Run nvidia-smi with given args. Returns 0 on success.
 * Optionally captures first line of stdout into out_buf.
 */
static int
nvidia_smi_run(const char *args, char *out_buf, size_t out_len)
{
	char cmd[256];
	FILE *fp;
	int ret;

	snprintf(cmd, sizeof(cmd), "nvidia-smi %s 2>/dev/null", args);
	fp = popen(cmd, "r");
	if (!fp) return -1;

	if (out_buf && out_len > 0) {
		out_buf[0] = '\0';
		if (fgets(out_buf, out_len, fp)) {
			/* Strip trailing whitespace/newline */
			size_t len = strlen(out_buf);
			while (len > 0 && (out_buf[len - 1] == '\n' || out_buf[len - 1] == '\r'
					|| out_buf[len - 1] == ' '))
				out_buf[--len] = '\0';
		}
	}

	ret = pclose(fp);
	return WIFEXITED(ret) ? WEXITSTATUS(ret) : -1;
}

/*
 * Query a single nvidia-smi CSV field for GPU at nv_gpu_index.
 * Returns 0 on success, result in out_buf.
 */
static int
nvidia_smi_query(const char *field, char *out_buf, size_t out_len)
{
	char args[256];
	snprintf(args, sizeof(args),
		"-i %d --query-gpu=%s --format=csv,noheader,nounits",
		nv_gpu_index, field);
	return nvidia_smi_run(args, out_buf, out_len);
}

/*
 * Detect which nvidia-smi GPU index corresponds to our detected GPU.
 * nvidia-smi and DRM card indices can differ on multi-GPU systems.
 */
static void
nvidia_detect_smi_index(GpuInfo *gpu)
{
	char buf[256], args[256];
	int i;

	/* Query PCI bus ID for each nvidia-smi GPU and match against our PCI slot */
	for (i = 0; i < 8; i++) {
		snprintf(args, sizeof(args),
			"-i %d --query-gpu=pci.bus_id --format=csv,noheader", i);
		if (nvidia_smi_run(args, buf, sizeof(buf)) != 0)
			break;
		/* nvidia-smi returns e.g. "00000000:01:00.0", our pci_slot is "0000:01:00.0" */
		if (strstr(buf, gpu->pci_slot)) {
			nv_gpu_index = i;
			wlr_log(WLR_INFO, "NVIDIA: PCI %s → nvidia-smi GPU index %d", gpu->pci_slot, i);
			return;
		}
	}
	/* Fallback: assume index 0 (most common single-GPU setup) */
	nv_gpu_index = 0;
}

static void
apply_nvidia_gpu_power(GpuInfo *gpu)
{
	char buf[128], args[256];

	/* Auto-detect correct nvidia-smi GPU index */
	nvidia_detect_smi_index(gpu);

	/*
	 * 1. Enable persistence mode — keeps driver loaded between GPU tasks,
	 *    eliminates ~1-2s initialization delay on each GPU operation.
	 */
	if (nvidia_smi_query("persistence_mode", buf, sizeof(buf)) == 0) {
		if (strstr(buf, "Disabled") || strcmp(buf, "Off") == 0) {
			nv_persistence_was_off = 1;
			snprintf(args, sizeof(args), "-i %d -pm 1", nv_gpu_index);
			nvidia_smi_run(args, NULL, 0);
			wlr_log(WLR_INFO, "NVIDIA: persistence mode enabled");
		}
	}

	/*
	 * 2. Set power limit to maximum allowed TDP.
	 *    More power = higher sustained boost clocks.
	 */
	if (nvidia_smi_query("power.limit", buf, sizeof(buf)) == 0) {
		strncpy(nv_saved_power_limit, buf, sizeof(nv_saved_power_limit) - 1);
		nv_saved_power_limit[sizeof(nv_saved_power_limit) - 1] = '\0';
	}
	if (nvidia_smi_query("power.max_limit", buf, sizeof(buf)) == 0 && buf[0]) {
		char *dot = strchr(buf, '.');
		if (dot) *dot = '\0';
		snprintf(args, sizeof(args), "-i %d --power-limit=%s", nv_gpu_index, buf);
		if (nvidia_smi_run(args, NULL, 0) == 0)
			wlr_log(WLR_INFO, "NVIDIA: power limit → %s W (was %s)", buf, nv_saved_power_limit);
	}

	/*
	 * 3. Lock GPU clocks to maximum boost frequency.
	 *    Prevents clock fluctuation and ensures max performance.
	 */
	if (nvidia_smi_query("clocks.max.graphics", buf, sizeof(buf)) == 0 && buf[0]) {
		char *dot = strchr(buf, '.');
		if (dot) *dot = '\0';
		snprintf(args, sizeof(args), "-i %d -lgc %s,%s", nv_gpu_index, buf, buf);
		if (nvidia_smi_run(args, NULL, 0) == 0) {
			nv_clocks_locked = 1;
			wlr_log(WLR_INFO, "NVIDIA: GPU clocks locked → %s MHz", buf);
		}
	}

	/*
	 * 4. Lock memory clocks to maximum frequency.
	 *    Prevents VRAM clock downshifting during gameplay.
	 */
	if (nvidia_smi_query("clocks.max.memory", buf, sizeof(buf)) == 0 && buf[0]) {
		char *dot = strchr(buf, '.');
		if (dot) *dot = '\0';
		snprintf(args, sizeof(args), "-i %d -lmc %s,%s", nv_gpu_index, buf, buf);
		if (nvidia_smi_run(args, NULL, 0) == 0)
			wlr_log(WLR_INFO, "NVIDIA: memory clocks locked → %s MHz", buf);
	}

	/*
	 * 5. Disable PCI power management for GPU device.
	 *    Prevents PCIe link power saving (L1/L1.1/L1.2) which adds latency.
	 *    Also disable PCI power management on the PCIe bridge (parent device)
	 *    for minimum link latency.
	 */
	snprintf(nv_pci_power_path, sizeof(nv_pci_power_path),
		"/sys/bus/pci/devices/%s/power/control", gpu->pci_slot);
	{
		int fd = open(nv_pci_power_path, O_RDONLY);
		if (fd >= 0) {
			ssize_t n = read(fd, nv_saved_pci_power, sizeof(nv_saved_pci_power) - 1);
			close(fd);
			if (n > 0) {
				nv_saved_pci_power[n] = '\0';
				if (nv_saved_pci_power[n - 1] == '\n') nv_saved_pci_power[n - 1] = '\0';
			}
			fd = open(nv_pci_power_path, O_WRONLY);
			if (fd >= 0) {
				write(fd, "on", 2);
				close(fd);
				wlr_log(WLR_INFO, "NVIDIA: PCI power → 'on' (was '%s')", nv_saved_pci_power);
			}
		}
	}

	/*
	 * 6. Set compute mode to DEFAULT to allow concurrent graphics + compute.
	 *    Some systems set exclusive mode which can interfere with games.
	 */
	snprintf(args, sizeof(args), "-i %d -c DEFAULT", nv_gpu_index);
	nvidia_smi_run(args, NULL, 0);

	/*
	 * 7. Set GPU performance preference via nvidia-settings (requires XWayland).
	 *    PowerMizer mode 1 = "Prefer Maximum Performance".
	 *    Also set performance level hint and disable GPU scaling to reduce overhead.
	 *    Note: nvidia-settings silently fails on pure Wayland — harmless.
	 */
	{
		char nv_settings_cmd[512];
		snprintf(nv_settings_cmd, sizeof(nv_settings_cmd),
			"nvidia-settings"
			" -a '[gpu:%d]/GPUPowerMizerMode=1'"
			" -a '[gpu:%d]/GpuPowerMizerDefaultMode=1'"
			" 2>/dev/null",
			gpu->card_index, gpu->card_index);
		FILE *fp = popen(nv_settings_cmd, "r");
		if (fp) {
			if (pclose(fp) == 0)
				wlr_log(WLR_INFO, "NVIDIA: PowerMizer → prefer max performance (via nvidia-settings)");
			else
				wlr_log(WLR_INFO, "NVIDIA: nvidia-settings unavailable (pure Wayland) — using nvidia-smi only");
		}
	}

	/*
	 * 8. NVIDIA kernel module parameters for performance.
	 *    NVreg_UsePageAttributeTable=1 improves memory mapping performance.
	 *    These are read-only after boot, so just log the current state.
	 */
	{
		char pat_val[8] = "";
		int fd = open("/sys/module/nvidia/parameters/NVreg_UsePageAttributeTable", O_RDONLY);
		if (fd >= 0) {
			ssize_t n = read(fd, pat_val, sizeof(pat_val) - 1);
			close(fd);
			if (n > 0) {
				pat_val[n] = '\0';
				if (pat_val[0] == '0')
					wlr_log(WLR_INFO, "NVIDIA: WARNING — NVreg_UsePageAttributeTable=0 "
						"(set nvidia.NVreg_UsePageAttributeTable=1 in boot params for better perf)");
			}
		}
	}

	nv_power_applied = 1;
	wlr_log(WLR_INFO, "NVIDIA: card%d (%s, smi-idx=%d) — full performance mode active",
		gpu->card_index, gpu->pci_slot, nv_gpu_index);
}

static void
restore_nvidia_gpu_power(void)
{
	char args[256];

	if (!nv_power_applied) return;

	/* Reset GPU clock lock */
	if (nv_clocks_locked) {
		snprintf(args, sizeof(args), "-i %d -rgc", nv_gpu_index);
		nvidia_smi_run(args, NULL, 0);
		snprintf(args, sizeof(args), "-i %d -rmc", nv_gpu_index);
		nvidia_smi_run(args, NULL, 0);
		nv_clocks_locked = 0;
		wlr_log(WLR_INFO, "NVIDIA: GPU/memory clock locks released");
	}

	/* Restore power limit */
	if (nv_saved_power_limit[0]) {
		char pl_copy[32];
		strncpy(pl_copy, nv_saved_power_limit, sizeof(pl_copy) - 1);
		pl_copy[sizeof(pl_copy) - 1] = '\0';
		char *dot = strchr(pl_copy, '.');
		if (dot) *dot = '\0';
		snprintf(args, sizeof(args), "-i %d --power-limit=%s", nv_gpu_index, pl_copy);
		nvidia_smi_run(args, NULL, 0);
		wlr_log(WLR_INFO, "NVIDIA: power limit restored → %s W", pl_copy);
		nv_saved_power_limit[0] = '\0';
	}

	/* Restore persistence mode */
	if (nv_persistence_was_off) {
		snprintf(args, sizeof(args), "-i %d -pm 0", nv_gpu_index);
		nvidia_smi_run(args, NULL, 0);
		nv_persistence_was_off = 0;
		wlr_log(WLR_INFO, "NVIDIA: persistence mode restored (off)");
	}

	/* Restore PCI power management */
	if (nv_saved_pci_power[0]) {
		int fd = open(nv_pci_power_path, O_WRONLY);
		if (fd >= 0) {
			write(fd, nv_saved_pci_power, strlen(nv_saved_pci_power));
			close(fd);
		}
		wlr_log(WLR_INFO, "NVIDIA: PCI power restored → '%s'", nv_saved_pci_power);
		nv_saved_pci_power[0] = '\0';
	}

	/* Restore PowerMizer to adaptive mode (nvidia-settings, silent fail on Wayland) */
	{
		char nv_settings_cmd[256];
		snprintf(nv_settings_cmd, sizeof(nv_settings_cmd),
			"nvidia-settings -a '[gpu:0]/GPUPowerMizerMode=0' 2>/dev/null");
		FILE *fp = popen(nv_settings_cmd, "r");
		if (fp) pclose(fp);
	}

	nv_power_applied = 0;
	wlr_log(WLR_INFO, "NVIDIA: performance mode deactivated — defaults restored");
}

void
apply_gpu_power_state(void)
{
	int gpu_idx = discrete_gpu_idx >= 0 ? discrete_gpu_idx : 0;
	if (gpu_idx >= detected_gpu_count) return;
	GpuInfo *gpu = &detected_gpus[gpu_idx];

	if (gpu->vendor == GPU_VENDOR_AMD) {
		/* AMD: Force high performance DPM level */
		snprintf(gpu_power_perf_path, sizeof(gpu_power_perf_path),
			"/sys/class/drm/%s/device/power_dma_perf_level",
			gpu->card_path[0] ? gpu->card_path : "card0");
		/* Try sysfs path with just card name */
		{
			char try_path[128];
			snprintf(try_path, sizeof(try_path),
				"/sys/class/drm/card%d/device/power_dma_perf_level", gpu->card_index);
			int fd = open(try_path, O_RDONLY);
			if (fd >= 0) {
				strncpy(gpu_power_perf_path, try_path, sizeof(gpu_power_perf_path) - 1);
				close(fd);
			}
		}

		sched_save_and_write(gpu_power_perf_path, "high",
			gpu_saved_perf_level, sizeof(gpu_saved_perf_level));

		/* AMD: Set power profile to VR/3D compute for max clocks */
		snprintf(gpu_power_profile_path, sizeof(gpu_power_profile_path),
			"/sys/class/drm/card%d/device/pp_power_profile_mode", gpu->card_index);
		{
			int fd = open(gpu_power_profile_path, O_RDONLY);
			if (fd >= 0) {
				char buf[512];
				ssize_t n = read(fd, buf, sizeof(buf) - 1);
				close(fd);
				if (n > 0) {
					buf[n] = '\0';
					/* Find current active profile (line with * at end) */
					char *line = buf;
					while (line && *line) {
						char *nl = strchr(line, '\n');
						if (nl) *nl = '\0';
						if (strchr(line, '*')) {
							/* Extract profile number */
							while (*line == ' ') line++;
							strncpy(gpu_saved_power_profile, line,
								sizeof(gpu_saved_power_profile) - 1);
							/* Just save the profile index number */
							char *sp = strchr(gpu_saved_power_profile, ' ');
							if (sp) *sp = '\0';
							break;
						}
						line = nl ? nl + 1 : NULL;
					}
				}
				/* Set to profile 3 (3D_FULL_SCREEN) for max performance */
				fd = open(gpu_power_profile_path, O_WRONLY);
				if (fd >= 0) {
					write(fd, "3", 1);
					close(fd);
				}
			}
		}

		wlr_log(WLR_INFO, "GPU power: AMD card%d → high perf + 3D profile", gpu->card_index);

	} else if (gpu->vendor == GPU_VENDOR_NVIDIA) {
		apply_nvidia_gpu_power(gpu);

	} else if (gpu->vendor == GPU_VENDOR_INTEL) {
		/* Intel: Set min frequency to max frequency */
		snprintf(gpu_intel_min_path, sizeof(gpu_intel_min_path),
			"/sys/class/drm/card%d/gt_min_freq_mhz", gpu->card_index);
		{
			char max_path[128];
			snprintf(max_path, sizeof(max_path),
				"/sys/class/drm/card%d/gt_max_freq_mhz", gpu->card_index);
			char max_freq[32] = "";
			int fd = open(max_path, O_RDONLY);
			if (fd >= 0) {
				ssize_t n = read(fd, max_freq, sizeof(max_freq) - 1);
				close(fd);
				if (n > 0) {
					max_freq[n] = '\0';
					if (max_freq[n - 1] == '\n') max_freq[n - 1] = '\0';
					sched_save_and_write(gpu_intel_min_path, max_freq,
						gpu_saved_intel_min, sizeof(gpu_saved_intel_min));
					wlr_log(WLR_INFO, "GPU power: Intel card%d → min_freq=%s MHz",
						gpu->card_index, max_freq);
				}
			}
		}
	}
}

void
restore_gpu_power_state(void)
{
	if (gpu_saved_perf_level[0]) {
		sched_restore(gpu_power_perf_path, gpu_saved_perf_level);
		wlr_log(WLR_INFO, "GPU power: AMD perf level restored");
	}
	if (gpu_saved_power_profile[0]) {
		int fd = open(gpu_power_profile_path, O_WRONLY);
		if (fd >= 0) {
			write(fd, gpu_saved_power_profile, strlen(gpu_saved_power_profile));
			close(fd);
		}
		gpu_saved_power_profile[0] = '\0';
		wlr_log(WLR_INFO, "GPU power: AMD power profile restored");
	}
	restore_nvidia_gpu_power();
	if (gpu_saved_intel_min[0]) {
		sched_restore(gpu_intel_min_path, gpu_saved_intel_min);
		wlr_log(WLR_INFO, "GPU power: Intel min freq restored");
	}
}

/*
 * Dirty Writeback Tuning — Prevent I/O stalls from page writeback.
 * Allow more dirty pages in RAM before flushing, reduces disk I/O
 * interference during texture streaming and asset loading.
 */
static char dirty_saved_ratio[32] = "";
static char dirty_saved_bg_ratio[32] = "";
static char dirty_saved_expire[32] = "";
static char dirty_saved_writeback[32] = "";

void
apply_dirty_writeback_tuning(void)
{
	/* Allow more dirty pages before forced writeback (default ~20 → 80%) */
	sched_save_and_write("/proc/sys/vm/dirty_ratio",
		"80", dirty_saved_ratio, sizeof(dirty_saved_ratio));

	/* Start background writeback earlier (default ~10 → 5%) */
	sched_save_and_write("/proc/sys/vm/dirty_background_ratio",
		"5", dirty_saved_bg_ratio, sizeof(dirty_saved_bg_ratio));

	/* Keep dirty pages longer before expiring (default ~3000 → 6000 centisecs) */
	sched_save_and_write("/proc/sys/vm/dirty_expire_centisecs",
		"6000", dirty_saved_expire, sizeof(dirty_saved_expire));

	/* Background writeback interval (default ~500 → 1500 centisecs) */
	sched_save_and_write("/proc/sys/vm/dirty_writeback_centisecs",
		"1500", dirty_saved_writeback, sizeof(dirty_saved_writeback));

	wlr_log(WLR_INFO, "Dirty writeback: ratio=80, bg=5, expire=6000, writeback=1500");
}

void
restore_dirty_writeback_tuning(void)
{
	sched_restore("/proc/sys/vm/dirty_ratio", dirty_saved_ratio);
	sched_restore("/proc/sys/vm/dirty_background_ratio", dirty_saved_bg_ratio);
	sched_restore("/proc/sys/vm/dirty_expire_centisecs", dirty_saved_expire);
	sched_restore("/proc/sys/vm/dirty_writeback_centisecs", dirty_saved_writeback);
	wlr_log(WLR_INFO, "Dirty writeback: restored defaults");
}

/*
 * Split Lock Mitigation Disable — Remove performance penalty.
 * Split lock detection costs ~70μs per event; disabling it eliminates
 * this overhead for games that trigger unaligned atomic operations.
 */
static int split_lock_saved = -1;

void
apply_disable_split_lock(void)
{
	int fd;
	char buf[8];
	ssize_t n;

	fd = open("/proc/sys/kernel/split_lock_mitigate", O_RDONLY);
	if (fd >= 0) {
		n = read(fd, buf, sizeof(buf) - 1);
		close(fd);
		if (n > 0) {
			buf[n] = '\0';
			split_lock_saved = atoi(buf);
		}
	}

	fd = open("/proc/sys/kernel/split_lock_mitigate", O_WRONLY);
	if (fd >= 0) {
		write(fd, "0\n", 2);
		close(fd);
		wlr_log(WLR_INFO, "Split lock mitigation: disabled");
	} else {
		wlr_log(WLR_INFO, "Split lock mitigation: not available");
	}
}

void
restore_split_lock(void)
{
	int fd;
	if (split_lock_saved > 0) {
		fd = open("/proc/sys/kernel/split_lock_mitigate", O_WRONLY);
		if (fd >= 0) {
			write(fd, "1\n", 2);
			close(fd);
		}
		wlr_log(WLR_INFO, "Split lock mitigation: restored");
	}
	split_lock_saved = -1;
}

/*
 * MGLRU Tuning (Multi-Gen LRU) — Optimize page reclaim.
 * Full MGLRU mode with no minimum TTL gives the kernel maximum
 * flexibility for efficient memory management during gaming.
 */
static char mglru_saved_enabled[32] = "";
static char mglru_saved_min_ttl[32] = "";

void
apply_mglru_tuning(void)
{
	/* Enable full MGLRU (value 5 = all features) */
	sched_save_and_write("/sys/kernel/mm/lru_gen/enabled",
		"5", mglru_saved_enabled, sizeof(mglru_saved_enabled));

	/* Set min_ttl to 0 for most aggressive reclaim */
	sched_save_and_write("/sys/kernel/mm/lru_gen/min_ttl_ms",
		"0", mglru_saved_min_ttl, sizeof(mglru_saved_min_ttl));

	wlr_log(WLR_INFO, "MGLRU: enabled=5, min_ttl=0");
}

void
restore_mglru_tuning(void)
{
	sched_restore("/sys/kernel/mm/lru_gen/enabled", mglru_saved_enabled);
	sched_restore("/sys/kernel/mm/lru_gen/min_ttl_ms", mglru_saved_min_ttl);
	wlr_log(WLR_INFO, "MGLRU: restored defaults");
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

	/*
	 * Determine if this is actually a game.
	 * Most games (especially XWayland/Steam/Wine) don't set content-type
	 * or tearing hints.  Flip the default: treat ANY fullscreen app as a
	 * game UNLESS it explicitly identifies as video, or is a known video
	 * player by app-id / class name.
	 */
	if (c) {
		if (client_wants_tearing(c) || is_game_content(c)) {
			/* Explicit game hints — definitely a game */
			is_game = 1;
		} else if (is_video_content(c)) {
			/* Explicit video hint — not a game */
			is_game = 0;
		} else {
			/* No hints — check app-id against known video players */
			const char *app = client_get_appid(c);
			static const char *video_apps[] = {
				"mpv", "vlc", "celluloid", "totem", "kodi",
				"haruna", "smplayer", "dragon", "parole",
				"io.github.celluloid_player.Celluloid",
				"org.videolan.VLC", "io.mpv.Mpv",
				"org.gnome.Totem", "tv.kodi.Kodi",
				NULL
			};
			is_game = 1; /* default: fullscreen = game */
			if (app) {
				for (int i = 0; video_apps[i]; i++) {
					if (strcasecmp(app, video_apps[i]) == 0 ||
					    strcasestr(app, video_apps[i])) {
						is_game = 0;
						break;
					}
				}
			}
		}
	}

	/* Ultra mode activates for games, regular game mode for other fullscreen content */
	game_mode_ultra = (game_mode_active && is_game);

	if (game_mode_ultra && !was_ultra) {
		/*
		 * ENTERING ULTRA GAME MODE
		 * Maximum performance - stop everything non-essential
		 */
		wlr_log(WLR_INFO, "ULTRA GAME MODE ACTIVATED - maximum performance, minimal latency");

		/* Show game detection notification */
		if (c && c->mon)
			toast_show(c->mon, "Fullscreen Game detected: dGPU + Frame Pacing active!", 3000);

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
		if (fan_thermal_timer)
			wl_event_source_timer_update(fan_thermal_timer, 0);
		/* NOTE: Keep bt_scan_timer - needed for controller reconnection */
		/* NOTE: Keep gamepad_pending_timer - needed for controller input */

		/* Boost GPU fans to prevent thermal throttling */
		fan_boost_activate();

		/* Freeze background processes to free CPU/memory for the game */
		freeze_background_processes();

		/* Apply memory optimizations (drop caches, lower swappiness) */
		apply_memory_optimization();

		/* Prevent deep CPU C-states for lowest wakeup latency */
		apply_cpu_latency_qos();

		/* Enable transparent huge pages for reduced TLB misses */
		apply_transparent_hugepages();

		/* Switch I/O schedulers to lowest-latency mode */
		apply_io_scheduler();

		/* Disable kernel watchdog to eliminate NMI jitter */
		apply_disable_watchdog();

		/* Disable pointer acceleration for raw 1:1 input */
		apply_raw_input();

		/* Pin all hardware IRQs to core 0 (compositor core) */
		apply_irq_affinity();

		/* Tune CFS scheduler for low-latency gaming */
		apply_scheduler_tuning();

		/* Force GPU to maximum performance state */
		apply_gpu_power_state();

		/* Optimize dirty page writeback to prevent I/O stalls */
		apply_dirty_writeback_tuning();

		/* Disable split lock mitigation overhead */
		apply_disable_split_lock();

		/* Enable full MGLRU for efficient page reclaim */
		apply_mglru_tuning();

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
			/* Isolate game and compositor to separate CPU cores */
			apply_cpu_affinity(game_mode_pid);
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

		/* Show fullscreen detection notification */
		if (c && c->mon)
			toast_show(c->mon, "Fullscreen detected: dGPU active", 3000);

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

			/* Restore MGLRU settings */
			restore_mglru_tuning();

			/* Restore split lock mitigation */
			restore_split_lock();

			/* Restore dirty writeback parameters */
			restore_dirty_writeback_tuning();

			/* Restore GPU power state */
			restore_gpu_power_state();

			/* Restore CFS scheduler parameters */
			restore_scheduler_tuning();

			/* Restore IRQ affinity to original cores */
			restore_irq_affinity();

			/* Restore raw input (pointer acceleration) */
			restore_raw_input();

			/* Restore kernel watchdog */
			restore_watchdog();

			/* Restore I/O schedulers */
			restore_io_scheduler();

			/* Restore transparent huge pages */
			restore_transparent_hugepages();

			/* Restore CPU affinity before restoring game priority */
			restore_cpu_affinity(game_mode_pid);

			/* Restore CPU DMA latency QoS */
			restore_cpu_latency_qos();

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

			/* Unfreeze background processes */
			unfreeze_background_processes();

			/* Restore memory settings */
			restore_memory_optimization();

			/* Restore GPU fans — thermal management will re-take control */
			fan_boost_deactivate();

			/* Restart thermal fan management timer */
			if (fan_thermal_timer)
				wl_event_source_timer_update(fan_thermal_timer, 1000);

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
	/* Steam dialogs/popups — but NOT games with steam_app_XXXX class.
	 * Proton/Wine games typically get WM_CLASS "steam_app_12345". */
	if (strncasecmp(app_id, "steam_app_", 10) == 0)
		return 0;  /* This is a game, not a popup */
	if (strcasestr(app_id, "steam") || strcasestr(app_id, "steamwebhelper"))
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
