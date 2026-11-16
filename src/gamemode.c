#include "nixlytile.h"
#include "client.h"
#include "run_cmd.h"
#include <pthread.h>
#include <poll.h>

/* pidfd for the ultra-mode keepalive check (PID-reuse safe) */
static int game_keepalive_pidfd = -1;

/*
 * ioprio constants (not always available in headers)
 */
#ifndef IOPRIO_CLASS_BE
#define IOPRIO_CLASS_BE		2
#define IOPRIO_WHO_PROCESS	1
#define IOPRIO_PRIO_VALUE(class, data)	(((class) << 13) | (data))
#endif

/* Forward declarations for helpers defined later in the file */
static int has_protected_ancestor(pid_t pid);
static int is_child_of(pid_t pid, pid_t ancestor);

/*
 * Privileged tuning lives in nixly-gametune.service (nixlyos:
 * modules/core/gametune.nix).  The compositor is an unprivileged user
 * process: every /proc/sys, /sys/kernel and /dev/cpu_dma_latency write this
 * file used to attempt failed with EACCES, so that half of game mode was a
 * silent no-op.  The unit holds /dev/cpu_dma_latency at 0 (no deep C-states)
 * and lowers swappiness for as long as it runs; a polkit rule lets the
 * gamemode group start/stop it.  Everything else that was poked at runtime —
 * CPU governor, THP, nmi_watchdog, split_lock, io scheduler, IRQ affinity —
 * is now static system config and needs nothing from us.
 */
static int gametune_active;

/* Whether the privileged helper unit exists at all. Probed once. */
static int
gametune_unit_present(void)
{
	static int present = -1;
	if (present >= 0)
		return present;

	static const char *const argv[] = {
		"systemctl", "cat", "nixly-gametune.service", NULL
	};
	present = run_cmd(argv) == 0;
	if (!present)
		nx_log(NX_ERROR, "gametune: nixly-gametune.service is not installed — "
			"C-state latency, CPU/GPU power limits and I/O tuning will NOT be "
			"applied. Enable the nixlytile NixOS module (nixosModules.default).");
	return present;
}

static void
gametune_start(void)
{
	if (!gametune_active)
		fanwatch_set_game(1);
	gametune_active = 1;
	if (!gametune_unit_present())
		return;

	/* Re-assert: daemon may have exited. */
	static const char *const argv[] = {
		"systemctl", "start", "--no-block", "nixly-gametune.service", NULL
	};
	if (run_cmd(argv) != 0)
		nx_log(NX_ERROR, "gametune: failed to start nixly-gametune.service "
			"(polkit rule missing? user must be in the 'gamemode' group)");
	else
		nx_log(NX_INFO, "gametune: nixly-gametune.service started");
}

static void
gametune_stop(void)
{
	if (!gametune_active)
		return;
	gametune_active = 0;
	fanwatch_set_game(0);
	if (!gametune_unit_present())
		return;

	/* Blocks until restore finishes. */
	static const char *const argv[] = {
		"systemctl", "stop", "nixly-gametune.service", NULL
	};
	run_cmd(argv);
	/* Restore may hold AC values. */
	powersave_battery_reassert_async();
	nx_log(NX_INFO, "gametune: nixly-gametune.service stopped");
}

/*
 * OOM protection for the game tree, applied by nixly-gametune through a
 * per-PID template unit — the same polkit-allowed path as the tuning unit.
 * The helper walks the game's process tree, saves every oom_score_adj and
 * sets -900, so a memory squeeze takes the browser instead of the game.
 */
static void
game_oom_protect(pid_t pid)
{
	char unit[64];

	if (pid <= 1 || !gametune_unit_present())
		return;

	snprintf(unit, sizeof(unit), "nixly-gameprio@%d.service", (int)pid);
	const char *const argv[] = { "systemctl", "start", "--no-block", unit, NULL };
	if (run_cmd(argv) != 0)
		nx_log(NX_ERROR, "gametune: failed to start %s — game stays "
			"unprotected from the OOM killer", unit);
	else
		game_mode_oom_applied = 1;
}

static void
game_oom_unprotect(pid_t pid)
{
	char unit[64];

	if (!game_mode_oom_applied)
		return;
	game_mode_oom_applied = 0;

	snprintf(unit, sizeof(unit), "nixly-gameprio@%d.service", (int)pid);
	const char *const argv[] = { "systemctl", "stop", "--no-block", unit, NULL };
	run_cmd(argv);
}

/* Play-press tuning until ultra. */
static int prelaunch_boost_active;
static struct wl_event_source *prelaunch_retry;
#define PRELAUNCH_SETTLE_MS 250

/* fx_track_start runs on the main loop while the 16 ms cover animation
 * ticks; the two systemctl invocations in gametune_start block 50-300 ms,
 * a visible launch hitch. Run them on a throwaway thread instead. */
static void *
prelaunch_start_thread(void *arg)
{
	(void)arg;
	gametune_start();
	return NULL;
}

static void *
prelaunch_stop_thread(void *arg)
{
	(void)arg;
	gametune_stop();
	return NULL;
}

void
game_prelaunch_boost(void)
{
	pthread_t t;

	if (prelaunch_boost_active)
		return;
	prelaunch_boost_active = 1;
	if (pthread_create(&t, NULL, prelaunch_start_thread, NULL) == 0)
		pthread_detach(t);
	else
		gametune_start();
}

static int
prelaunch_retry_cb(void *data)
{
	(void)data;
	game_prelaunch_release();
	return 0;
}

void
game_prelaunch_release(void)
{
	pthread_t t;
	int hold = launchfx_cover_headstart_remaining();

	if (!prelaunch_boost_active)
		return;
	/* Ultra imminent; recheck after it. */
	if (hold > 0) {
		if (!prelaunch_retry)
			prelaunch_retry = wl_event_loop_add_timer(event_loop, prelaunch_retry_cb, NULL);
		if (prelaunch_retry)
			wl_event_source_timer_update(prelaunch_retry, hold + PRELAUNCH_SETTLE_MS);
		return;
	}
	prelaunch_boost_active = 0;
	/* Ultra mode owns tuning now. */
	if (game_mode_ultra)
		return;
	if (pthread_create(&t, NULL, prelaunch_stop_thread, NULL) == 0)
		pthread_detach(t);
	else
		gametune_stop();
}

/* Apply nice + ioprio to every thread of a process (both are per-task
 * on Linux; the TGID-leader calls in apply/restore_game_priority miss
 * threads spawned before game mode engaged). */
static void
game_priority_all_threads(pid_t pid, int nice_val, int ioprio)
{
	char task_dir[64];
	DIR *d;
	struct dirent *ent;

	snprintf(task_dir, sizeof(task_dir), "/proc/%d/task", pid);
	d = opendir(task_dir);
	if (!d)
		return;
	while ((ent = readdir(d))) {
		pid_t tid;
		if (ent->d_name[0] < '0' || ent->d_name[0] > '9')
			continue;
		tid = (pid_t)atoi(ent->d_name);
		setpriority(PRIO_PROCESS, tid, nice_val);
		syscall(__NR_ioprio_set, IOPRIO_WHO_PROCESS, tid, ioprio);
	}
	closedir(d);
}

#ifndef SCHED_BATCH
#define SCHED_BATCH 3
#endif
#ifndef SCHED_IDLE
#define SCHED_IDLE 5
#endif

/* ananicy-style daemons classify the Steam launcher tree as background
 * (sched idle) and every game forked from it inherits the class; their
 * name-based reclassification misses UE titles, which rename comm at
 * startup (Squad → "GameThread").  nice is meaningless inside
 * SCHED_IDLE, so lift the whole thread group back to SCHED_OTHER before
 * boosting.  IDLE→OTHER needs no privilege for own-uid tasks. */
static void
game_policy_repair(pid_t pid)
{
	char task_dir[64];
	DIR *d;
	struct dirent *ent;
	struct sched_param sp = {0};
	int pol, fixed = 0;

	snprintf(task_dir, sizeof(task_dir), "/proc/%d/task", pid);
	d = opendir(task_dir);
	if (!d)
		return;
	while ((ent = readdir(d))) {
		pid_t tid;
		if (ent->d_name[0] < '0' || ent->d_name[0] > '9')
			continue;
		tid = (pid_t)atoi(ent->d_name);
		pol = sched_getscheduler(tid);
		if ((pol == SCHED_IDLE || pol == SCHED_BATCH) &&
		    sched_setscheduler(tid, SCHED_OTHER, &sp) == 0)
			fixed++;
	}
	closedir(d);
	if (fixed)
		nx_log(NX_INFO,
			"Game priority: lifted %d SCHED_IDLE/BATCH tasks to SCHED_OTHER for PID %d",
			fixed, pid);
}

void
apply_game_priority(pid_t pid)
{
	if (pid <= 1)
		return;

	game_policy_repair(pid);

	/*
	 * Set nice value to -10 (requires CAP_SYS_NICE or root).
	 * Stronger than the old -5 for better CPU scheduling priority.
	 */
	if (setpriority(PRIO_PROCESS, pid, -10) == 0) {
		game_mode_nice_applied = 1;
		nx_log(NX_INFO, "Game priority: set nice=-10 for PID %d", pid);
	} else {
		nx_log(NX_INFO, "Game priority: setpriority failed for PID %d: %s",
			pid, strerror(errno));
	}

	/*
	 * Set I/O priority to best-effort class 0 (highest non-RT).
	 * NOT IOPRIO_CLASS_RT: that needs CAP_SYS_ADMIN and always failed
	 * with EPERM from an unprivileged compositor.  BE/0 is allowed for
	 * our own uid and is what mq-deadline/bfq actually act on — NVMe runs
	 * the `none` scheduler, where ioprio is ignored either way.
	 */
	if (syscall(__NR_ioprio_set, IOPRIO_WHO_PROCESS, pid,
		    IOPRIO_PRIO_VALUE(IOPRIO_CLASS_BE, 0)) == 0) {
		game_mode_ioclass_applied = 1;
		nx_log(NX_INFO, "Game priority: set ioprio BE/0 for PID %d", pid);
	} else {
		nx_log(NX_INFO, "Game priority: ioprio_set failed for PID %d: %s",
			pid, strerror(errno));
	}

	/*
	 * Linux nice and ioprio are PER-TASK: the calls above only hit the
	 * thread-group leader, so a game's render/RHI/audio threads (spawned
	 * before game mode engaged) would keep nice 0 / default ioprio.
	 * Walk /proc/PID/task so every thread gets the boost — same pattern
	 * as set_process_affinity_all_threads.
	 */
	game_priority_all_threads(pid, -10,
		IOPRIO_PRIO_VALUE(IOPRIO_CLASS_BE, 0));

	/* OOM protection goes through the root helper: lowering oom_score_adj
	 * needs CAP_SYS_RESOURCE, so writing it from here always failed with
	 * EACCES and left the game as killable as anything else. */
	game_oom_protect(pid);

	/* No governor handling here: scaling_governor is root-owned (the write
	 * always failed) and the system now runs `performance` permanently. */
}

void
restore_game_priority(pid_t pid)
{
	if (pid <= 1)
		return;

	/* Restore nice value to 0 (normal) */
	if (game_mode_nice_applied) {
		setpriority(PRIO_PROCESS, pid, 0);
		game_mode_nice_applied = 0;
		nx_log(NX_INFO, "Game priority: restored nice=0 for PID %d", pid);
	}

	/* Restore I/O priority to best-effort class, priority 4 (normal) */
	if (game_mode_ioclass_applied) {
		syscall(__NR_ioprio_set, IOPRIO_WHO_PROCESS, pid,
			IOPRIO_PRIO_VALUE(IOPRIO_CLASS_BE, 4));
		game_mode_ioclass_applied = 0;
		nx_log(NX_INFO, "Game priority: restored ioprio BE/4 for PID %d", pid);
	}

	/* Per-thread restore, mirroring the per-thread apply */
	game_priority_all_threads(pid, 0, IOPRIO_PRIO_VALUE(IOPRIO_CLASS_BE, 4));

	game_oom_unprotect(pid);
}

/* Session plumbing games block on. */
static int
essential_process(const char *comm)
{
	static const char *const prefix[] = {
		"nixly", "Xwayland", "xwayland", "pipewire", "wireplumber", "pulseaudio",
		"dbus-", "xdg-", "systemd", "gvfs", "at-spi", "gnome-keyring", "gcr-",
		"ssh-agent", "sshd", "polkit", "fusermount", "wine",
	};
	static const char *const exact[] = { "bash", "zsh", "fish", "sh", "dash" };
	size_t n = strlen(comm);

	if (n > 4 && !strcmp(comm + n - 4, ".exe"))
		return 1;
	for (size_t i = 0; i < LENGTH(prefix); i++)
		if (!strncmp(comm, prefix[i], strlen(prefix[i])))
			return 1;
	for (size_t i = 0; i < LENGTH(exact); i++)
		if (!strcmp(comm, exact[i]))
			return 1;
	return 0;
}

/*
 * Push every other user process down to SCHED_IDLE while game runs.
 * SCHED_IDLE = lowest CPU class, only runs when no SCHED_OTHER/RR/FIFO
 * task wants the core.  Game stays on SCHED_OTHER nice=-10 → effectively
 * monopolises CPU under contention.  Compositor + audio + dbus + shells
 * remain on SCHED_OTHER (whitelist) so input/audio/system-bus stays
 * responsive.
 *
 * No deadlock risk: SCHED_IDLE is preempted by anything, including
 * the compositor's own RT loops if it ever needs them.
 */
#ifndef SCHED_IDLE
#define SCHED_IDLE 5
#endif

#define MAX_LOWERED_PIDS 4096
static pid_t lowered_pids[MAX_LOWERED_PIDS];
static int   lowered_pid_count = 0;

void
lower_competing_processes(pid_t game_pid)
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

	lowered_pid_count = 0;

	dir = opendir("/proc");
	if (!dir)
		return;

	while ((ent = readdir(dir))) {
		if (ent->d_name[0] < '0' || ent->d_name[0] > '9')
			continue;

		pid = (pid_t)atoi(ent->d_name);
		if (pid <= 1 || pid == our_pid || pid == game_pid)
			continue;

		/* User-owned only */
		snprintf(path, sizeof(path), "/proc/%d/status", pid);
		fp = fopen(path, "r");
		if (!fp)
			continue;

		int is_our_uid = 0;
		while (fgets(uid_line, sizeof(uid_line), fp)) {
			if (strncmp(uid_line, "Uid:", 4) == 0) {
				uid_t real_uid;
				if (sscanf(uid_line + 4, "%u", &real_uid) == 1
				    && real_uid == our_uid)
					is_our_uid = 1;
				break;
			}
		}
		fclose(fp);
		if (!is_our_uid)
			continue;

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

		if (essential_process(comm))
			continue;

		/* Don't touch protected ancestry (Steam tree) or game subtree */
		if (has_protected_ancestor(pid))
			continue;
		if (game_pid > 1 && is_child_of(pid, game_pid))
			continue;

		/* Keep pre-idle tasks idle. */
		if (sched_getscheduler(pid) == SCHED_IDLE)
			continue;

		struct sched_param sp = {0};
		if (sched_setscheduler(pid, SCHED_IDLE, &sp) == 0) {
			if (lowered_pid_count < MAX_LOWERED_PIDS)
				lowered_pids[lowered_pid_count++] = pid;
		}
	}
	closedir(dir);

	nx_log(NX_INFO, "CPU isolate: SCHED_IDLE applied to %d processes",
		lowered_pid_count);
}

void
restore_competing_processes(void)
{
	struct sched_param sp = {0};
	int i;

	if (lowered_pid_count == 0)
		return;

	for (i = 0; i < lowered_pid_count; i++)
		sched_setscheduler(lowered_pids[i], SCHED_OTHER, &sp);

	nx_log(NX_INFO, "CPU isolate: restored %d processes to SCHED_OTHER",
		lowered_pid_count);
	lowered_pid_count = 0;
}

/*
 * Undo the two things that leave the session unusable if the compositor dies
 * without running cleanup(): SIGSTOPped background processes and processes
 * pushed to SCHED_IDLE. Everything else game mode touches (GPU clocks, the
 * gametune unit, VRR) either self-restores or merely wastes power.
 *
 * Called from a fatal signal handler, so nothing here may allocate, take a
 * lock or touch stdio: only raw syscalls, and only on state that was already
 * written down before the crash.
 */
void
gm_emergency_restore(void)
{
	struct sched_param sp = {0};
	int i;

	for (i = 0; i < frozen_pid_count; i++)
		kill(frozen_pids[i], SIGCONT);
	frozen_pid_count = 0;

	for (i = 0; i < lowered_pid_count; i++)
		sched_setscheduler(lowered_pids[i], SCHED_OTHER, &sp);
	lowered_pid_count = 0;
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
 * Process names whose entire subtrees are protected during game mode.
 * Any process with one of these as an ancestor will not be frozen.
 * This protects Wine/Proton siblings (wineserver, bwrap, etc.) that
 * are part of the Steam process tree but not children of game_mode_pid.
 */
static const char *tree_protect[] = {
	"steam", "discord", "Discord", "vesktop", "Vesktop",
	"webcord", "WebCord", "armcord", "ArmCord", "legcord", "Legcord",
	NULL
};

/*
 * Check if a PID has an ancestor whose comm matches a tree_protect entry.
 * Walks up the ppid chain reading /proc/<pid>/stat.
 */
static int
has_protected_ancestor(pid_t pid)
{
	char path[64];
	char comm[64];
	FILE *fp;
	char line[256];
	pid_t cur;
	int depth = 0;
	int i;

	cur = pid;
	while (cur > 1 && depth < 32) {
		/* Read comm of current pid */
		snprintf(path, sizeof(path), "/proc/%d/comm", cur);
		fp = fopen(path, "r");
		if (!fp)
			return 0;
		comm[0] = '\0';
		if (fgets(comm, sizeof(comm), fp)) {
			char *nl = strchr(comm, '\n');
			if (nl) *nl = '\0';
		}
		fclose(fp);

		for (i = 0; tree_protect[i]; i++) {
			if (strcmp(comm, tree_protect[i]) == 0)
				return 1;
		}

		/* Walk to parent */
		snprintf(path, sizeof(path), "/proc/%d/stat", cur);
		fp = fopen(path, "r");
		if (!fp)
			return 0;
		if (!fgets(line, sizeof(line), fp)) {
			fclose(fp);
			return 0;
		}
		fclose(fp);
		char *cp = strrchr(line, ')');
		if (!cp)
			return 0;
		pid_t ppid;
		if (sscanf(cp + 2, "%*c %d", &ppid) != 1)
			return 0;
		cur = ppid;
		depth++;
	}
	return 0;
}

#ifndef MADV_PAGEOUT
#define MADV_PAGEOUT 21
#endif
#ifndef __NR_pidfd_open
#define __NR_pidfd_open 434
#endif
#ifndef __NR_process_madvise
#define __NR_process_madvise 440
#endif

/*
 * Force frozen process pages to swap using process_madvise(MADV_PAGEOUT).
 * This frees physical RAM that SIGSTOP alone leaves resident.
 */
static void
reclaim_frozen_memory(void)
{
	struct iovec iov;
	int pidfd;

	iov.iov_base = 0;
	iov.iov_len = (size_t)-1;  /* entire address space */

	for (int i = 0; i < frozen_pid_count; i++) {
		pidfd = syscall(__NR_pidfd_open, frozen_pids[i], 0);
		if (pidfd < 0)
			continue;
		syscall(__NR_process_madvise, pidfd, &iov, (size_t)1, MADV_PAGEOUT, (unsigned int)0);
		close(pidfd);
	}
	nx_log(NX_INFO, "Reclaimed memory from %d frozen processes", frozen_pid_count);
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

	/* Whitelist of process comm names that should never be frozen.
	 * Steam/Discord subtrees are protected by has_protected_ancestor().
	 * Must match lower_competing_processes(): SIGSTOPping the user
	 * systemd manager, sshd session processes or interactive shells
	 * freezes SSH sessions and stalls user-unit management for the
	 * whole game. */
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

		if (essential_process(comm))
			continue;

		/* Don't freeze any process in a Steam/Discord subtree */
		if (has_protected_ancestor(pid))
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
	nx_log(NX_INFO, "Froze %d background processes for game mode", frozen_pid_count);

	/* Push frozen process pages to swap to free physical RAM */
	if (frozen_pid_count > 0)
		reclaim_frozen_memory();
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

	nx_log(NX_INFO, "Unfroze %d background processes", frozen_pid_count);
	frozen_pid_count = 0;
}

/*
 * CPU Affinity — Core isolation.
 * Pin the compositor to core 0 and the game to cores 1..N-1 to prevent
 * them from competing for the same CPU cache lines and scheduler slots.
 */
/* Apply a cpu mask to every thread of a process — sched_setaffinity on
 * the pid alone only moves the thread-group leader, leaving a game's
 * render/audio threads unpinned. */
static void
set_process_affinity_all_threads(pid_t pid, const cpu_set_t *set)
{
	char task_dir[64];
	DIR *d;
	struct dirent *ent;

	sched_setaffinity(pid, sizeof(*set), set);

	snprintf(task_dir, sizeof(task_dir), "/proc/%d/task", pid);
	d = opendir(task_dir);
	if (!d)
		return;
	while ((ent = readdir(d))) {
		if (ent->d_name[0] < '0' || ent->d_name[0] > '9')
			continue;
		sched_setaffinity((pid_t)atoi(ent->d_name), sizeof(*set), set);
	}
	closedir(d);
}

/*
 * Split the online CPUs into a compositor set and a game set.
 * Returns 0 when the machine is too small to isolate anything.
 *
 * The compositor set is cpu0 plus ALL of its SMT siblings — one physical
 * core — which is also where IRQBALANCE_BANNED_CPUS keeps every hardware
 * interrupt.  Reading thread_siblings_list is the point: cpu0 and cpu1 are
 * two *different* physical cores under both Intel and modern AMD
 * enumeration, so the old fixed "compositor 0-1 / game 2..N-1" split gave
 * the compositor two whole cores and then handed the game their SMT
 * siblings to fight over.  Adapts to any core count and either topology.
 */
static int
gm_core_split(cpu_set_t *comp, cpu_set_t *game)
{
	long ncores = sysconf(_SC_NPROCESSORS_ONLN);
	FILE *fp;
	char buf[256];
	long i;

	/* Under 8 logical CPUs, giving one core away costs more than the
	 * isolation buys — leave the scheduler alone. */
	if (ncores < 8)
		return 0;

	CPU_ZERO(comp);
	fp = fopen("/sys/devices/system/cpu/cpu0/topology/thread_siblings_list", "r");
	if (fp) {
		if (fgets(buf, sizeof(buf), fp)) {
			/* "0,8" (SMT pair), "0-1" (range) or "0" (no SMT) */
			char *tok = strtok(buf, ",\n");
			while (tok) {
				int a = -1, b = -1;
				if (sscanf(tok, "%d-%d", &a, &b) == 2) {
					for (i = a; i <= b && i < ncores; i++)
						CPU_SET(i, comp);
				} else if (sscanf(tok, "%d", &a) == 1 && a < ncores) {
					CPU_SET(a, comp);
				}
				tok = strtok(NULL, ",\n");
			}
		}
		fclose(fp);
	}
	if (CPU_COUNT(comp) == 0)
		CPU_SET(0, comp);

	/* Never leave the compositor on a single logical CPU: with every IRQ
	 * pinned to the same core and SCHED_RR on, a one-CPU pin made tight
	 * retry loops un-preemptable (the retroarch 10-bit scanout freeze). */
	if (CPU_COUNT(comp) < 2)
		CPU_SET(1, comp);

	CPU_ZERO(game);
	for (i = 0; i < ncores; i++)
		if (!CPU_ISSET(i, comp))
			CPU_SET(i, game);

	return CPU_COUNT(game) > 0;
}

/*
 * Asymmetric-L3 detection (X3D dual-CCD): group online CPUs by their L3
 * domain (cache/index3/shared_cpu_list) and compare sizes.  When one
 * domain's L3 is strictly larger than another's, that is the V-Cache CCD
 * and the game belongs there — the equivalent of what Windows' Game Bar
 * pinning does.  Symmetric parts (equal L3s, single L3, no index3) return
 * 0 and the game keeps every CPU.  Threads created later inherit the mask.
 */
#define GM_L3_MAX_DOMAINS 8

static int
gm_parse_cpu_list(const char *buf, cpu_set_t *set, long ncores)
{
	char tmp[256];
	char *tok;
	long i;

	snprintf(tmp, sizeof(tmp), "%s", buf);
	CPU_ZERO(set);
	tok = strtok(tmp, ",\n");
	while (tok) {
		int a = -1, b = -1;
		if (sscanf(tok, "%d-%d", &a, &b) == 2) {
			for (i = a; i <= b && i < ncores; i++)
				CPU_SET(i, set);
		} else if (sscanf(tok, "%d", &a) == 1 && a >= 0 && a < ncores) {
			CPU_SET(a, set);
		}
		tok = strtok(NULL, ",\n");
	}
	return CPU_COUNT(set) > 0;
}

static int
gm_vcache_set(cpu_set_t *out)
{
	long ncores = sysconf(_SC_NPROCESSORS_ONLN);
	cpu_set_t dom_set[GM_L3_MAX_DOMAINS];
	long dom_kb[GM_L3_MAX_DOMAINS];
	int ndom = 0;
	cpu_set_t seen;
	char path[128], buf[256];
	FILE *fp;
	long cpu, kb;
	int d, best = 0, smaller = 0;

	if (ncores < 12)	/* cache CCD must keep enough threads for a game */
		return 0;

	CPU_ZERO(&seen);
	for (cpu = 0; cpu < ncores && ndom < GM_L3_MAX_DOMAINS; cpu++) {
		if (CPU_ISSET(cpu, &seen))
			continue;
		snprintf(path, sizeof(path),
			"/sys/devices/system/cpu/cpu%ld/cache/index3/shared_cpu_list",
			cpu);
		fp = fopen(path, "r");
		if (!fp)
			return 0;	/* no L3 info — stay hands-off */
		if (!fgets(buf, sizeof(buf), fp)) {
			fclose(fp);
			return 0;
		}
		fclose(fp);
		if (!gm_parse_cpu_list(buf, &dom_set[ndom], ncores))
			return 0;

		snprintf(path, sizeof(path),
			"/sys/devices/system/cpu/cpu%ld/cache/index3/size", cpu);
		fp = fopen(path, "r");
		if (!fp)
			return 0;
		kb = 0;
		if (fscanf(fp, "%ld", &kb) != 1 || kb <= 0) {
			fclose(fp);
			return 0;
		}
		fclose(fp);
		dom_kb[ndom] = kb;

		CPU_OR(&seen, &seen, &dom_set[ndom]);
		ndom++;
	}
	if (ndom < 2)
		return 0;

	for (d = 1; d < ndom; d++)
		if (dom_kb[d] > dom_kb[best])
			best = d;
	for (d = 0; d < ndom; d++)
		if (dom_kb[d] < dom_kb[best])
			smaller = 1;
	/* Equal L3s = symmetric CCDs; cross-CCD pinning is not a clear win
	 * there, so only the asymmetric (V-Cache) case pins. */
	if (!smaller || CPU_COUNT(&dom_set[best]) < 8)
		return 0;

	*out = dom_set[best];
	return 1;
}

void
apply_cpu_affinity(pid_t game_pid)
{
	cpu_set_t all_set;
	cpu_set_t vset;
	long ncores = sysconf(_SC_NPROCESSORS_ONLN);

	if (game_pid <= 1 || ncores < 1)
		return;

	/* Asymmetric dual-CCD (X3D): the game's working set belongs in the
	 * big L3.  Everything else (ananicy'd background, compositor's own
	 * pin) is unaffected; restore_cpu_affinity lifts the mask on exit. */
	if (gm_vcache_set(&vset)) {
		set_process_affinity_all_threads(game_pid, &vset);
		game_mode_affinity_applied = 1;
		nx_log(NX_INFO,
			"CPU affinity: game PID %d → V-Cache CCD (%d of %ld CPUs)",
			game_pid, CPU_COUNT(&vset), ncores);
		return;
	}

	/*
	 * The game gets every CPU. An earlier version fenced it off the
	 * compositor's physical core, which cost a CPU-bound game a full core
	 * (2 of 16 threads here) to solve a problem the SCHED_RR boost already
	 * solves: at real-time priority the compositor preempts the game
	 * whenever it needs to run, so it cannot be starved by a SCHED_OTHER
	 * process no matter which CPU that process occupies.
	 *
	 * The compositor still pins itself to physical core 0 (see
	 * update_game_mode) for cache locality — that pin restricts only the
	 * compositor, not the game.
	 *
	 * This also re-applies the full mask over anything a previous game
	 * session or an external tool left behind.
	 */
	CPU_ZERO(&all_set);
	for (long i = 0; i < ncores; i++)
		CPU_SET(i, &all_set);
	set_process_affinity_all_threads(game_pid, &all_set);

	game_mode_affinity_applied = 1;
	nx_log(NX_INFO, "CPU affinity: game PID %d → all %ld CPUs", game_pid, ncores);
}

void
restore_cpu_affinity(pid_t game_pid)
{
	if (!game_mode_affinity_applied) return;
	int ncores = sysconf(_SC_NPROCESSORS_ONLN);

	cpu_set_t all_set;
	CPU_ZERO(&all_set);
	for (int i = 0; i < ncores; i++)
		CPU_SET(i, &all_set);
	if (game_pid > 1)
		set_process_affinity_all_threads(game_pid, &all_set);

	game_mode_affinity_applied = 0;
	nx_log(NX_INFO, "CPU affinity: restored to all cores");
}

/*
 * Raw Input Mode — Disable pointer acceleration.
 * Gives 1:1 mouse movement, essential for FPS/competitive games.
 */
void
apply_raw_input(void)
{
	/* Kjører på bg-worker-tråden; lås mot hotplug-prune på main-tråden
	 * (unplugget device = freed libinput-peker uten prune+lås). */
	pthread_mutex_lock(&pointer_devices_lock);
	for (int i = 0; i < pointer_device_count; i++) {
		if (libinput_device_config_accel_is_available(pointer_devices[i])) {
			libinput_device_config_accel_set_profile(pointer_devices[i],
				LIBINPUT_CONFIG_ACCEL_PROFILE_FLAT);
		}
	}
	pthread_mutex_unlock(&pointer_devices_lock);
	game_mode_raw_input_applied = 1;
	nx_log(NX_INFO, "Raw input: disabled pointer acceleration (%d devices)", pointer_device_count);
}

void
restore_raw_input(void)
{
	if (!game_mode_raw_input_applied) return;
	pthread_mutex_lock(&pointer_devices_lock);
	for (int i = 0; i < pointer_device_count; i++) {
		if (libinput_device_config_accel_is_available(pointer_devices[i])) {
			libinput_device_config_accel_set_profile(pointer_devices[i], accel_profile);
			libinput_device_config_accel_set_speed(pointer_devices[i], accel_speed);
		}
	}
	pthread_mutex_unlock(&pointer_devices_lock);
	game_mode_raw_input_applied = 0;
	nx_log(NX_INFO, "Raw input: restored pointer acceleration profile");
}

/*
 * AMD alone can raise another process's GPU queue priority
 * (DRM_IOCTL_AMDGPU_SCHED), and only on a DRM master fd.
 */
#include <libdrm/amdgpu_drm.h>

#ifndef __NR_pidfd_getfd
#define __NR_pidfd_getfd 438
#endif

static int gpu_sched_local_fd = -1;  /* game DRM fd, while boosted */
static int gpu_sched_master_fd = -1;

static const GpuInfo *
game_gpu(void)
{
	int idx = discrete_gpu_idx >= 0 ? discrete_gpu_idx : 0;

	return idx < detected_gpu_count ? &detected_gpus[idx] : NULL;
}

/* Game's fd for GPU, or -1. */
static int
find_process_drm_fd(pid_t pid, const GpuInfo *gpu)
{
	char dir_path[64], target[256];
	DIR *d;
	struct dirent *ent;
	ssize_t len;
	int found = -1;

	snprintf(dir_path, sizeof(dir_path), "/proc/%d/fd", pid);
	d = opendir(dir_path);
	if (!d)
		return -1;
	while (found < 0 && (ent = readdir(d)) != NULL) {
		len = ent->d_name[0] == '.' ? -1 :
			readlinkat(dirfd(d), ent->d_name, target, sizeof(target) - 1);
		if (len <= 0)
			continue;
		target[len] = '\0';
		if (!strcmp(target, gpu->render_path) || !strcmp(target, gpu->card_path))
			found = atoi(ent->d_name);
	}
	closedir(d);
	return found;
}

/* Backend-owned AMD master fd. */
static int
game_gpu_master_fd(void)
{
	const GpuInfo *gpu = game_gpu();
	struct stat want, have;
	Monitor *m;

	if (!gpu || gpu->vendor != GPU_VENDOR_AMD || stat(gpu->card_path, &want) < 0)
		return -1;
	wl_list_for_each(m, &mons, link) {
		int fd = m->nx_output ? nx_backend_get_drm_fd(m->nx_output->backend) : -1;

		if (fd >= 0 && fstat(fd, &have) == 0 && have.st_rdev == want.st_rdev)
			return fd;
	}
	return -1;
}

static int
sched_override(int master_fd, int fd, int32_t priority)
{
	union drm_amdgpu_sched sched = {0};

	sched.in.op = AMDGPU_SCHED_OP_PROCESS_PRIORITY_OVERRIDE;
	sched.in.fd = fd;
	sched.in.priority = priority;
	return drmIoctl(master_fd, DRM_IOCTL_AMDGPU_SCHED, &sched);
}

/* Our dup of game fd. */
static int
borrow_drm_fd(pid_t pid, const GpuInfo *gpu)
{
	int num = find_process_drm_fd(pid, gpu), pidfd, fd;

	if (num < 0)
		return -1;
	pidfd = (int)syscall(__NR_pidfd_open, pid, 0);
	if (pidfd < 0)
		return -1;
	fd = (int)syscall(__NR_pidfd_getfd, pidfd, num, 0);
	close(pidfd);
	return fd;
}

/* VERY_HIGH wins eviction; HIGH fallback. */
static void
apply_gpu_sched_priority(pid_t pid, int master_fd)
{
	const GpuInfo *gpu = game_gpu();
	int fd = master_fd >= 0 && gpu && pid > 1 ? borrow_drm_fd(pid, gpu) : -1;

	if (fd < 0)
		return;
	if (sched_override(master_fd, fd, AMDGPU_CTX_PRIORITY_VERY_HIGH) < 0 &&
	    sched_override(master_fd, fd, AMDGPU_CTX_PRIORITY_HIGH) < 0) {
		nx_log(NX_INFO, "GPU sched: AMDGPU_SCHED failed: %s", strerror(errno));
		close(fd);
		return;
	}
	gpu_sched_local_fd = fd;
	gpu_sched_master_fd = master_fd;
	nx_log(NX_INFO, "GPU sched: raised AMD queue priority for PID %d", pid);
}

static void
restore_gpu_sched_priority(void)
{
	if (gpu_sched_local_fd < 0)
		return;
	sched_override(gpu_sched_master_fd, gpu_sched_local_fd, AMDGPU_CTX_PRIORITY_NORMAL);
	close(gpu_sched_local_fd);
	gpu_sched_local_fd = -1;
}

/*
 * Retro emulators must NEVER trigger game mode — no statusbar hide,
 * no process freeze, no GPU clock lock, no focus override, no "Game
 * Mode Active" OSD. Match on both the Wayland app_id (if set) and
 * /proc/PID/comm so we catch clients that haven't announced their
 * app_id yet.
 */
static const char *retro_emulator_apps[] = {
	"retroarch", "org.libretro.RetroArch",
	"dolphin-emu", "org.DolphinEmu.dolphin-emu",
	"pcsx2", "rpcs3", "cemu", "yuzu",
	"desmume", "mgba", "ppsspp", "citra",
	NULL,
};

static int
retro_match_string(const char *s)
{
	int i;
	if (!s || !*s)
		return 0;
	for (i = 0; retro_emulator_apps[i]; i++) {
		if (strcasestr(s, retro_emulator_apps[i]))
			return 1;
	}
	return 0;
}

static int
retro_check_single_pid(pid_t pid)
{
	char path[64], buf[512];
	FILE *fp;
	ssize_t n;
	int fd;
	size_t len;

	if (pid <= 1)
		return 0;

	/* /proc/PID/comm — kernel task name (max 15 chars) */
	snprintf(path, sizeof(path), "/proc/%d/comm", pid);
	fp = fopen(path, "r");
	if (fp) {
		if (fgets(buf, sizeof(buf), fp)) {
			len = strlen(buf);
			if (len > 0 && buf[len - 1] == '\n')
				buf[len - 1] = '\0';
			if (retro_match_string(buf)) {
				fclose(fp);
				return 1;
			}
		}
		fclose(fp);
	}

	/* /proc/PID/exe — real executable path (catches wrappers w/ short comm) */
	snprintf(path, sizeof(path), "/proc/%d/exe", pid);
	n = readlink(path, buf, sizeof(buf) - 1);
	if (n > 0) {
		buf[n] = '\0';
		if (retro_match_string(buf))
			return 1;
	}

	/* /proc/PID/cmdline — full argv, NUL-separated */
	snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
	fd = open(path, O_RDONLY);
	if (fd >= 0) {
		n = read(fd, buf, sizeof(buf) - 1);
		close(fd);
		if (n > 0) {
			ssize_t i;
			buf[n] = '\0';
			for (i = 0; i < n; i++)
				if (buf[i] == '\0')
					buf[i] = ' ';
			if (retro_match_string(buf))
				return 1;
		}
	}

	return 0;
}

static int
is_retro_emulator_pid(pid_t pid)
{
	char path[64], stat_comm[64];
	FILE *fp;
	pid_t cur = pid;
	int depth = 0;

	/* Walk up to 10 ancestors — catches emulator cores / shell wrappers
	 * whose own PID no longer advertises retroarch but whose parent does. */
	while (cur > 1 && depth < 10) {
		if (retro_check_single_pid(cur))
			return 1;

		snprintf(path, sizeof(path), "/proc/%d/stat", cur);
		fp = fopen(path, "r");
		if (!fp)
			return 0;
		pid_t ppid = 0;
		if (fscanf(fp, "%*d (%63[^)]) %*c %d", stat_comm, &ppid) != 2) {
			fclose(fp);
			return 0;
		}
		fclose(fp);
		cur = ppid;
		depth++;
	}
	return 0;
}

static int
retro_emulator_probe(Client *c)
{
	const char *app;
	pid_t pid;
	int i;

	app = client_get_appid(c);
	if (app && *app) {
		for (i = 0; retro_emulator_apps[i]; i++) {
			if (strcasecmp(app, retro_emulator_apps[i]) == 0 ||
			    strcasestr(app, retro_emulator_apps[i]))
				return 1;
		}
	}

	pid = client_get_pid(c);
	if (pid > 1 && is_retro_emulator_pid(pid))
		return 1;

	/* Ancestry check: clients launched under the retroarch session we
	 * started via the gamepad menu — covers subprocesses whose comm or
	 * app_id doesn't advertise themselves as retroarch yet. */
	if (pid > 1 && retro_session_pid > 1) {
		if (kill(retro_session_pid, 0) != 0 && errno == ESRCH) {
			retro_session_pid = 0;
		} else if (is_child_of(pid, retro_session_pid)) {
			return 1;
		}
	}

	return 0;
}

/* Memoized wrapper: the probe walks /proc ancestry, and looks_like_game()
 * lands here per vblank when an overlay covers a fullscreen game — that
 * was thousands of /proc opens per second. The verdict is frozen once the
 * app_id exists (early X11 maps have none yet). */
int
is_retro_emulator_client(Client *c)
{
	int r;

	if (!c)
		return 0;
	if (c->retro_verdict)
		return c->retro_verdict > 0;
	r = retro_emulator_probe(c);
	if (r || client_get_appid(c))
		c->retro_verdict = r ? 1 : -1;
	return r;
}

/* RetroArch keeps one window for both menu and running content, so the
 * promotion needs a signal that separates the two.  The window title
 * used to serve: menu "RetroArch 1.x.y", content "… - core - game".
 * It doesn't — a fullscreen RetroArch playing a PS2 title was still
 * classified cls=- on the HTPC, which kept the 10-bit render format
 * (an explicit direct-scanout blocker) and left the game composited
 * with no frame pacing.
 *
 * The loaded libretro core is the real signal: RetroArch dlopens
 * <core>_libretro.so when content starts and it shows up in the
 * process map.  No core mapped = the plain menu, which keeps the
 * blanket retro exclusion. */
static int
retro_core_loaded(pid_t pid)
{
	char path[64], line[512];
	FILE *f;
	int found = 0;

	snprintf(path, sizeof path, "/proc/%d/maps", (int)pid);
	if (!(f = fopen(path, "r")))
		return 0;
	while (fgets(line, sizeof line, f)) {
		if (strstr(line, "_libretro.so")) {
			found = 1;
			break;
		}
	}
	fclose(f);
	return found;
}

int
retro_content_running(Client *c)
{
	const char *app;
	pid_t pid;
	uint64_t now;

	if (!c)
		return 0;
	app = client_get_appid(c);
	if (!app || !strcasestr(app, "retroarch"))
		return 0;
	pid = client_get_pid(c);
	if (pid <= 1)
		return 0;

	/* Memoized: the map walk is far too heavy for the per-vblank
	 * callers, and content load/unload is a human-speed event. */
	now = monotonic_msec();
	if (c->retro_content_ms && now - c->retro_content_ms < 500)
		return c->retro_content_verdict;

	c->retro_content_verdict = retro_core_loaded(pid);
	c->retro_content_ms = now ? now : 1;
	return c->retro_content_verdict;
}

/* The retro exclusion, made content-aware: RetroArch sitting in its
 * menu still blocks game mode, but once content runs the full game
 * path (ultra tuning, framepace, VRR) engages. */
int
retro_blocks_game(Client *c)
{
	return is_retro_emulator_client(c) && !retro_content_running(c);
}

/* RetroArch flips menu<->content inside ONE toplevel, so the pacing
 * regime chosen at fullscreen-enter (setfullscreen) goes stale: the
 * menu-only scanout lock stayed on for the whole game session and
 * nothing armed game pacing.  Called from the classify cache the
 * moment the game flag flips in place.  Promotion drops the lock and
 * starts sampling fresh; demotion restores native menu presentation. */
void
retro_content_reclass(Monitor *m, Client *c, int game_now)
{
	if (!m || !m->nx_output || !is_retro_emulator_client(c))
		return;
	if (game_now) {
		if (m->retro_scanout_lock) {
			nx_output_lock_attach_render(m->nx_output, false);
			m->retro_scanout_lock = 0;
		}
		set_adaptive_sync(m, 1);
	} else {
		if (!m->retro_scanout_lock) {
			nx_output_lock_attach_render(m->nx_output, true);
			m->retro_scanout_lock = 1;
		}
		set_adaptive_sync(m, 0);
		disable_game_vrr(m);
	}
	/* Stale locks/rings from the previous regime must not throttle the
	 * new one (autolock's low ring spans 45 s). */
	autolock_reset(m);
	schedule_game_mode_update();
	nx_log(NX_INFO, "retro reclass: %s → %s on %s",
		client_get_appid(c) ? client_get_appid(c) : "?",
		game_now ? "content (game path)" : "menu (native)",
		m->nx_output->name);
}

/* ── menu max-refresh hold (RetroArch / nixlymedia) ──────────────── */

/* While the focused client is a RetroArch or nixlymedia *menu*, its
 * monitor runs at the highest refresh rate the current resolution
 * offers.  The hold is dropped the moment content runs (game mode owns
 * the output) or video plays (the cadence path owns the refresh rate),
 * restoring the mode that was active before. */
static Monitor *menuhz_mon;
static struct nx_output_mode *menuhz_saved;
static struct wl_event_source *menuhz_timer;
/* Mode the output rejected — without this memory the 1 s re-check
 * retried the same failing modeset forever (HDMI retrain/blank loop
 * in every menu on TVs whose max mode fails link training). */
static struct nx_output_mode *menuhz_failed;

static int
menu_maxhz_client(Client *c)
{
	const char *app = c ? client_get_appid(c) : NULL;

	if (app && (strcasestr(app, "retroarch") ||
			strcasestr(app, "nixlymedia")))
		return 1;
	/* Steam Big Picture and its guide-button menu: the menu is a
	 * separate steamwebhelper toplevel, so when it takes focus the
	 * hold used to drop — BPM ran at max Hz but the menu did not.
	 * is_steam_popup() excludes actual games (steam_app_*). */
	if (is_steam_client(c) || is_steam_popup(c))
		return 1;
	return is_retro_emulator_client(c);
}

static int
same_resolution(struct nx_output_mode *a, struct nx_output_mode *b)
{
	return a && b && a->width == b->width && a->height == b->height;
}

static void
menuhz_commit(Monitor *m, struct nx_output_mode *mode)
{
	struct nx_output_state st;

	if (!m || !m->nx_output || !mode ||
			m->nx_output->current_mode == mode)
		return;
	nx_output_state_init(&st);
	nx_output_state_set_mode(&st, mode);
	if (nx_output_test_state(m->nx_output, &st)
			&& nx_output_commit_state(m->nx_output, &st)) {
		nx_log(NX_INFO, "menuhz: %s → %dx%d@%dmHz",
				m->nx_output->name, mode->width, mode->height,
				mode->refresh);
		/* Same broadcast as every other modeset path — without it
		 * output-management clients (nixlycc) show a stale mode. */
		struct nx_output_configuration_v1 *config =
			nx_output_configuration_v1_create();
		struct nx_output_configuration_head_v1 *config_head =
			nx_output_configuration_head_v1_create(config,
					m->nx_output);
		config_head->state.mode = mode;
		nx_output_manager_v1_set_configuration(output_mgr, config);
		updatemons(NULL, NULL);
	} else {
		nx_log(NX_ERROR, "menuhz: %s rejected %dx%d@%dmHz",
				m->nx_output->name, mode->width, mode->height,
				mode->refresh);
		menuhz_failed = mode;
	}
	nx_output_state_finish(&st);
}

/* Monitor is being destroyed: drop the menu-refresh hold statics so the
 * 1 s timer never dereferences the freed Monitor (TVs power-cycling HDMI
 * make unplug-while-held reachable in practice). */
void
menuhz_forget(Monitor *m)
{
	if (menuhz_mon != m)
		return;
	menuhz_mon = NULL;
	menuhz_saved = NULL;
	menuhz_failed = NULL;
}

static int menuhz_timer_cb(void *data);

void
menu_maxhz_update(void)
{
	Client *f = selmon ? focustop(selmon) : NULL;
	Monitor *want = NULL;

	if (f && !game_mode_active && menu_maxhz_client(f) &&
			!retro_content_running(f) && !is_video_content(f))
		want = f->mon ? f->mon : selmon;

	if (want != menuhz_mon) {
		/* Others own video and resolution. */
		if (menuhz_mon && menuhz_saved && !game_mode_active
				&& !menuhz_mon->video_mode_active
				&& !menuhz_mon->vrr_active
				&& same_resolution(menuhz_saved,
					menuhz_mon->nx_output->current_mode))
			menuhz_commit(menuhz_mon, menuhz_saved);
		menuhz_mon = want;
		menuhz_saved = NULL;
		menuhz_failed = NULL;
		if (want && want->nx_output && want->nx_output->current_mode) {
			struct nx_output_mode *cur =
				want->nx_output->current_mode;
			struct nx_output_mode *max =
				find_mode(want->nx_output,
						cur->width, cur->height, 0);

			menuhz_saved = cur;
			nx_log(NX_INFO, "menuhz: hold on %s (%s), cur "
					"%dx%d@%dmHz, max at this res %dmHz",
					want->nx_output->name,
					f && client_get_appid(f)
						? client_get_appid(f) : "?",
					cur->width, cur->height, cur->refresh,
					max ? max->refresh : 0);
		}
	}
	if (menuhz_mon && menuhz_mon->nx_output &&
			!menuhz_mon->video_mode_active && !menuhz_mon->vrr_active &&
			menuhz_mon->nx_output->current_mode) {
		struct nx_output_mode *cur =
			menuhz_mon->nx_output->current_mode;
		struct nx_output_mode *max = find_mode(menuhz_mon->nx_output,
				cur->width, cur->height, 0);

		if (max && max != cur && max != menuhz_failed)
			menuhz_commit(menuhz_mon, max);
	}
	/* video/content state changes have no dedicated event — re-check
	 * once a second while the hold (or a candidate) exists */
	if (menuhz_mon) {
		if (!menuhz_timer)
			menuhz_timer = wl_event_loop_add_timer(event_loop,
					menuhz_timer_cb, NULL);
		if (menuhz_timer)
			wl_event_source_timer_update(menuhz_timer, 1000);
	}
}

static int
menuhz_timer_cb(void *data)
{
	menu_maxhz_update();
	return 0;
}

/*
 * Background worker thread for game mode system operations.
 * All blocking I/O (process freeze/unfreeze, sysfs writes, DRM ioctls,
 * memory reclaim, fan control) runs here instead of on the compositor
 * event loop, preventing compositor freezes during tag switches.
 */
static pthread_t gm_bg_thread;
static pthread_mutex_t gm_bg_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gm_bg_cond = PTHREAD_COND_INITIALIZER;
static int gm_bg_alive;
static int gm_bg_wake;
static int gm_bg_desired_ultra;     /* posted, under gm_bg_mutex */
static pid_t gm_bg_pid;
static int gm_bg_master_fd = -1;
static int gm_bg_applied_ultra;     /* worker-owned */
static pid_t gm_bg_applied_pid;

/* Game boosts come first. */
static void
boost_game(pid_t pid, int master_fd)
{
	if (pid <= 1)
		return;
	apply_game_priority(pid);
	apply_cpu_affinity(pid);
	apply_gpu_sched_priority(pid, master_fd);
}

static void
unboost_game(pid_t pid)
{
	restore_gpu_sched_priority();
	if (pid <= 1)
		return;
	restore_cpu_affinity(pid);
	restore_game_priority(pid);
}

static void
gm_ultra_enter(pid_t pid, int master_fd)
{
	nx_log(NX_INFO, "gm-bg: entering ultra (pid=%d)", pid);
	boost_game(pid, master_fd);
	gametune_start();
	apply_raw_input();
	freeze_background_processes();
	if (pid > 1)
		lower_competing_processes(pid);
	nx_log(NX_INFO, "gm-bg: ultra enter complete");
}

/* Teardown, also at compositor exit. */
static void
gm_ultra_exit(pid_t pid)
{
	nx_log(NX_INFO, "gm-bg: exiting ultra (pid=%d)", pid);
	restore_competing_processes();
	restore_raw_input();
	unboost_game(pid);
	gametune_stop();
	unfreeze_background_processes();
	nx_log(NX_INFO, "gm-bg: ultra exit complete");
}

/* New game while ultra stays. */
static void
gm_ultra_retarget(pid_t pid, int master_fd)
{
	nx_log(NX_INFO, "gm-bg: ultra moves from pid %d to %d", gm_bg_applied_pid, pid);
	restore_competing_processes();
	unboost_game(gm_bg_applied_pid);
	boost_game(pid, master_fd);
	if (pid > 1)
		lower_competing_processes(pid);
}

static void
gm_bg_apply(int want, pid_t pid, int master_fd)
{
	if (want && !gm_bg_applied_ultra)
		gm_ultra_enter(pid, master_fd);
	else if (want && pid != gm_bg_applied_pid)
		gm_ultra_retarget(pid, master_fd);
	else if (!want && gm_bg_applied_ultra)
		gm_ultra_exit(gm_bg_applied_pid);
	gm_bg_applied_ultra = want;
	gm_bg_applied_pid = want ? pid : 0;
}

static void *
gm_bg_worker_func(void *arg)
{
	(void)arg;
	pthread_mutex_lock(&gm_bg_mutex);
	while (gm_bg_alive) {
		int want, master_fd;
		pid_t pid;

		while (!gm_bg_wake && gm_bg_alive)
			pthread_cond_wait(&gm_bg_cond, &gm_bg_mutex);
		if (!gm_bg_alive)
			break;
		gm_bg_wake = 0;
		want = gm_bg_desired_ultra;
		pid = gm_bg_pid;
		master_fd = gm_bg_master_fd;
		pthread_mutex_unlock(&gm_bg_mutex);
		gm_bg_apply(want, pid, master_fd);
		pthread_mutex_lock(&gm_bg_mutex);
		if (gm_bg_desired_ultra != want || gm_bg_pid != pid)
			gm_bg_wake = 1;
	}
	pthread_mutex_unlock(&gm_bg_mutex);
	return NULL;
}

static void
gm_bg_post(int ultra, pid_t pid)
{
	int master_fd = ultra ? game_gpu_master_fd() : -1;

	pthread_mutex_lock(&gm_bg_mutex);
	gm_bg_desired_ultra = ultra;
	gm_bg_pid = pid;
	gm_bg_master_fd = master_fd;
	gm_bg_wake = 1;
	pthread_cond_signal(&gm_bg_cond);
	pthread_mutex_unlock(&gm_bg_mutex);
}

void
gm_bg_init(void)
{
	gm_bg_alive = 1;
	pthread_create(&gm_bg_thread, NULL, gm_bg_worker_func, NULL);
	pthread_setname_np(gm_bg_thread, "gm-bg-worker");
}

void
gm_bg_cleanup(void)
{
	pthread_mutex_lock(&gm_bg_mutex);
	gm_bg_alive = 0;
	pthread_cond_signal(&gm_bg_cond);
	pthread_mutex_unlock(&gm_bg_mutex);
	pthread_join(gm_bg_thread, NULL);
	if (gm_bg_applied_ultra)
		gm_ultra_exit(gm_bg_applied_pid);
}

static struct wl_event_source *game_mode_debounce_timer;

static int
game_mode_debounce_cb(void *data)
{
	(void)data;
	update_game_mode();
	return 0;
}

void
schedule_game_mode_update(void)
{
	/* Same trigger set (map/unmap/view/fullscreen/ws switch) drives the
	 * HTPC workspace freezer: thaw synchronously, refreeze debounced. */
	wsfreeze_poke();
	if (!game_mode_debounce_timer)
		game_mode_debounce_timer = wl_event_loop_add_timer(event_loop,
			game_mode_debounce_cb, NULL);
	if (game_mode_debounce_timer)
		wl_event_source_timer_update(game_mode_debounce_timer, 50);
}

/* Running game beats other fullscreen. */
static Client *
fullscreen_focus_client(void)
{
	Client *c, *first = NULL;

	wl_list_for_each(c, &clients, link) {
		if (!c->isfullscreen || !client_surface(c)->mapped || !VISIBLEON(c, c->mon) ||
		    (c->fs_ws && c->fs_ws != c->mon->active_ws))
			continue;
		if (c == game_mode_client ||
		    (!is_steam_client(c) && !is_steam_popup(c) && looks_like_game(c)))
			return c;
		first = first ? first : c;
	}
	return first;
}

void
update_game_mode(void)
{
	int was_active = game_mode_active;
	int was_ultra = game_mode_ultra;
	Client *c = fullscreen_focus_client();
	Monitor *m;
	int is_game = 0;
	int retro_menu = 0;

	/*
	 * Emulators in their menus must not engage game mode.  Match on
	 * Wayland app_id AND /proc/PID/comm — app_id may be unset on the
	 * first mapnotify, but the process name is always available.
	 * RetroArch with content running is a real game: the exclusion
	 * lifts and the full ultra/framepace path engages.
	 */
	Client *retro_c = NULL;
	if (c && retro_blocks_game(c)) {
		retro_menu = 1;
		retro_c = c;
		c = NULL;
	}

	/*
	 * Guard: if ultra game mode is active and the game process is still
	 * running, NEVER deactivate game mode even if the fullscreen client
	 * is temporarily not found (e.g., surface unmap during loading,
	 * brief focus loss, or tag switch).  Deactivating would reset GPU
	 * power state, clock locks, and fan boost — causing severe FPS drops.
	 * Exception: RetroArch back in its menu — same process, but the
	 * game is over and ultra must wind down.
	 */
	if (was_ultra && !c && !retro_menu && game_mode_pid > 1) {
		int alive;
		if (game_keepalive_pidfd >= 0) {
			/* pidfd: POLLIN = process exited.  Immune to PID reuse
			 * and unreaped zombies, unlike kill(pid, 0). */
			struct pollfd pfd = { .fd = game_keepalive_pidfd,
					.events = POLLIN };
			alive = poll(&pfd, 1, 0) == 0;
		} else {
			alive = kill(game_mode_pid, 0) == 0;
		}
		if (alive)
			return; /* game still running — keep ultra mode */
	}

	/*
	 * Positive game detection.
	 *
	 * This used to accept Steam titles only, which meant a game started
	 * from Lutris, Heroic, Bottles, umu, gamescope, bare Wine/Proton or a
	 * native Linux binary got no GPU clock lock, no background freeze and
	 * no RT compositor — the whole tuning path simply never ran.
	 *
	 * looks_like_game() is the same classifier the fullscreen/VRR/scanout
	 * path already trusts, and it is conservative in exactly the places
	 * that matter: browsers, retro emulators, the Steam client itself and
	 * its popups all return 0 before any positive signal is considered.
	 * Using it here makes the aggressive tuning follow the same definition
	 * of "this is a game" as the rest of the compositor, on every machine
	 * and every launcher.
	 */
	if (c && !is_steam_client(c) && !is_steam_popup(c) && looks_like_game(c))
		is_game = 1;

	game_mode_active = is_game;
	game_mode_client = is_game ? c : NULL;
	game_mode_ultra  = is_game;

	/* Menu max-Hz hold for RetroArch/nixlymedia/Steam menus.  Must run
	 * AFTER the reclassification above: it is gated on
	 * !game_mode_active, and with the stale value a RetroArch game
	 * exit left the menu stuck on the game's mode (e.g. 60 Hz) until
	 * the next workspace switch. */
	menu_maxhz_update();

	if (game_mode_ultra && !was_ultra) {
		/*
		 * ENTERING ULTRA GAME MODE
		 * Maximum performance - stop everything non-essential
		 */
		nx_log(NX_INFO, "ULTRA GAME MODE ACTIVATED - maximum performance, minimal latency");

		/* The cover normally started at the fullscreen transition in
		 * setfullscreen(); this is the backstop for games that reach
		 * ultra mode without one.  No-op if a cover is up or already
		 * played for this client.
		 * Never for retro emulators: promotion happens inside an
		 * already-presenting fullscreen toplevel (menu → content), and
		 * an opaque cover over it starves the explicit-sync client —
		 * never sampled → no release point → 100 ms WSI timeout per
		 * frame (10 fps) under up to 20 s of black. */
		if (!is_retro_emulator_client(c))
			launchfx_fullscreen_starting(c);

		/* Give the cover a 1 s head start: ultra mode flips on direct
		 * scanout and heavy tuning, which would put the game on screen
		 * before the grow animation has played.  Hold activation until
		 * the cover has run, then retry via the debounce timer. */
		{
			int hold = launchfx_cover_headstart_remaining();
			if (hold > 0) {
				game_mode_active = 0;
				game_mode_ultra = 0;
				game_mode_client = NULL;
				if (!game_mode_debounce_timer)
					game_mode_debounce_timer =
						wl_event_loop_add_timer(event_loop,
							game_mode_debounce_cb, NULL);
				if (game_mode_debounce_timer)
					wl_event_source_timer_update(
						game_mode_debounce_timer, hold);
				return;
			}
		}

		/*
		 * Get game PID FIRST — before freeze_background_processes()
		 * so the game and its children are excluded from SIGSTOP.
		 */
		game_mode_pid = client_get_pid(c);

		/* pidfd for the keepalive check: kill(pid, 0) succeeds for
		 * recycled PIDs and unreaped zombies, which kept ultra mode
		 * (frozen background, locked clocks) stuck after game exit. */
		if (game_keepalive_pidfd >= 0)
			close(game_keepalive_pidfd);
		game_keepalive_pidfd = game_mode_pid > 1
			? (int)syscall(__NR_pidfd_open, game_mode_pid, 0) : -1;

		/* statusbar / network / launcher / gamepad timers removed */

		/*
		 * Dispatch blocking system operations to background thread.
		 * Process freeze, sysfs tuning, GPU priority, fan boost — all
		 * performed off the compositor event loop to prevent input/render
		 * freezes during tag switches.
		 */
		gm_bg_post(1, game_mode_pid);

		/* Hide statusbar on ALL monitors to save GPU compositing time.
		 * arrangelayers is what actually parks the bar tree — without it
		 * the bar scene node stays enabled under the game and blocks
		 * direct scanout for clients without a covering opaque region. */
		wl_list_for_each(m, &mons, link) {
			m->showbar = 0;
			arrangelayers(m);
			/* Any live toast ("Game Mode On" from the launch cover
			 * included) blocks direct scanout until it expires —
			 * drop them now that the game owns the screen. */
			osd_purge_mon(m);
		}

		/* Pin the compositor thread to physical core 0 (all its SMT
		 * threads).  Runs HERE (main thread) because
		 * sched_setaffinity(0, ...) affects the calling thread — on
		 * the gm-bg worker it pinned the worker.  Same split the game
		 * affinity uses; see gm_core_split(). */
		{
			cpu_set_t comp_set, game_set;
			if (gm_core_split(&comp_set, &game_set) &&
			    sched_setaffinity(0, sizeof(comp_set), &comp_set) == 0) {
				compositor_pin_applied = 1;
				nx_log(NX_INFO, "Compositor pinned to %d CPU(s) of physical core 0",
					CPU_COUNT(&comp_set));
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
		nx_log(NX_INFO, "Game mode activated (non-game fullscreen) - pausing background tasks");


	} else if (!game_mode_active && was_active) {
		/*
		 * EXITING GAME MODE - restore everything
		 */
		nx_log(NX_INFO, "Game mode deactivated - restoring normal operation");

		/* Restore additional timers if we were in ultra mode */
		if (was_ultra) {
			/* Re-enable showbar on all monitors (never in htpc
			 * mode — the bar stays hidden there permanently) */
			wl_list_for_each(m, &mons, link) {
				m->showbar = htpc_mode_active ? 0 : 1;
				/* Game mode cleared showbar without an arrange, so
				 * layoutstatusbar never armed its re-show render and
				 * the status timers stayed disarmed — force both. */
				statusbar_force_refresh(m);
				/* Disable game VRR on all monitors when leaving game mode */
				if (m->game_vrr_active)
					disable_game_vrr(m);
				arrangelayers(m);
			}

			/* Drain tray status queries deferred during the game and
			 * re-render the now-visible tray. */
			tray_refresh_stale();

			if (config_rewatch_timer)
				wl_event_source_timer_update(config_rewatch_timer, 2000);

			/* Un-pin compositor (main thread, like the pin) */
			if (compositor_pin_applied) {
				int ncores = sysconf(_SC_NPROCESSORS_ONLN);
				cpu_set_t all_set;
				CPU_ZERO(&all_set);
				for (int i = 0; i < ncores; i++)
					CPU_SET(i, &all_set);
				sched_setaffinity(0, sizeof(all_set), &all_set);
				compositor_pin_applied = 0;
			}

			/*
			 * Dispatch blocking restore operations to background thread.
			 * Sysfs writes, process unfreeze, memory restore, fan —
			 * all off the compositor event loop.
			 */
			gm_bg_post(0, game_mode_pid);

			/* Restart thermal fan management timer */
			if (fan_thermal_timer)
				wl_event_source_timer_update(fan_thermal_timer, 1000);

			nx_log(NX_INFO, "Ultra game mode deactivated - full system restored");

		} else {
			/*
			 * Non-ultra exit (video/browser fullscreen).
			 * Clean up orphaned video VRR that was enabled
			 * during fullscreen but never disabled on tag switch.
			 */
			wl_list_for_each(m, &mons, link) {
				if (m->vrr_active)
					disable_vrr_video_mode(m);
				if (m->video_cadence_active) {
					m->video_cadence_active = 0;
					m->video_cadence_accum = 0.0f;
					m->video_cadence_counter = 0;
				}
			}
		}

		/* RetroArch content → menu is a game exit with no
		 * setfullscreen transition, so the gamescan resolution and
		 * autolock refresh would survive into the menu (menuhz only
		 * raises refresh at the game's resolution).  Restore the
		 * menu to bestmode explicitly. */
		if (retro_menu && retro_c && retro_c->mon) {
			gamescan_restore(retro_c->mon);
			autolock_reset(retro_c->mon);
		}

		game_mode_ultra = 0;

		/* Show exit notification for ultra game mode only.  After the
		 * flags are cleared — osd_show() drops toasts while game mode
		 * is up. */
		if (was_ultra && selmon)
			show_hz_osd(selmon, "Game Mode Off");

		game_mode_client = NULL;
		game_mode_pid = 0;
		if (game_keepalive_pidfd >= 0) {
			close(game_keepalive_pidfd);
			game_keepalive_pidfd = -1;
		}
	}

	/* dcspit instrument screens follow game mode: shown while the game is
	 * up, gone the moment it is not — the output is a plain desktop again. */
	if (game_mode_active != was_active)
		instruments_update();
}

