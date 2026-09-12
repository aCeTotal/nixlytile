/*
 * wsfreeze.c — HTPC deep-freeze for off-screen work.
 *
 * Two regimes, both htpc-mode only (htpc_mode_active):
 *
 *   1. Hidden workspaces: every mapped client whose workspace is not the
 *      monitor's active one gets its whole process tree SIGSTOPped after
 *      a short debounce.  audio-focus.nix already mutes their streams,
 *      but a muted RetroArch/CEF still renders, decodes and feeds the
 *      PipeWire graph — frozen it costs nothing and its stalled streams
 *      let WirePlumber idle the HDMI sink (less noise on the receiver).
 *
 *   2. Dark session: when no real output is enabled and awake (TV on
 *      another HDMI input, DPMS off), EVERY client tree is frozen and
 *      the default sink muted; first frame after wake thaws and unmutes.
 *
 * Freezing is by client-pid subtree (snapshot of /proc), never the whole
 * user: the compositor, its supervisors (htpc-* loops just block in
 * wait()), PipeWire and Xwayland keep running.  A group whose leader has
 * a VISIBLE descendant is never frozen (a running game is a child of the
 * hidden Steam window's pid).  While game_mode_active, gamemode.c owns
 * background freezing and the hidden-workspace pass stands down.
 *
 * Thaw is synchronous in wsfreeze_poke() (hooked from
 * schedule_game_mode_update and updatemons), so a workspace switch or TV
 * wake resumes the target app before its next frame is needed.
 * wsfreeze_thaw_all() is async-signal-safe for the fatal-signal path.
 */
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>

#include "nixlytile.h"
#include "client.h"
#include "util.h"

#define WSF_MAX_GROUPS   32
#define WSF_GROUP_PIDS   128
#define WSF_SNAP_MAX     2048
#define WSF_DELAY_MS     3000   /* debounce before freezing anything */

typedef struct {
	pid_t leader;               /* the client pid the group was built from */
	int n;
	pid_t pids[WSF_GROUP_PIDS];
} WsfGroup;

static WsfGroup groups[WSF_MAX_GROUPS];
static int ngroups;
static int session_dark;        /* dark-session freeze engaged (sink muted) */
static struct wl_event_source *wsf_timer;

/* /proc snapshot (our uid only), rebuilt once per freeze pass */
static pid_t snap_pid[WSF_SNAP_MAX], snap_ppid[WSF_SNAP_MAX];
static int snap_n;

static int
wsf_read_status(pid_t pid, uid_t *uid, pid_t *ppid)
{
	char path[64], line[256];
	FILE *fp;
	int got = 0;

	snprintf(path, sizeof(path), "/proc/%d/status", pid);
	fp = fopen(path, "r");
	if (!fp)
		return 0;
	while (got != 3 && fgets(line, sizeof(line), fp)) {
		if (!strncmp(line, "Uid:", 4)) {
			unsigned u;
			if (sscanf(line + 4, "%u", &u) == 1) {
				*uid = (uid_t)u;
				got |= 1;
			}
		} else if (!strncmp(line, "PPid:", 5)) {
			int p;
			if (sscanf(line + 5, "%d", &p) == 1) {
				*ppid = (pid_t)p;
				got |= 2;
			}
		}
	}
	fclose(fp);
	return got == 3;
}

static void
wsf_snapshot(void)
{
	DIR *dir;
	struct dirent *ent;
	uid_t our_uid = getuid();

	snap_n = 0;
	dir = opendir("/proc");
	if (!dir)
		return;
	while ((ent = readdir(dir)) && snap_n < WSF_SNAP_MAX) {
		pid_t pid, ppid;
		uid_t uid;
		if (ent->d_name[0] < '0' || ent->d_name[0] > '9')
			continue;
		pid = (pid_t)atoi(ent->d_name);
		if (pid <= 1)
			continue;
		if (!wsf_read_status(pid, &uid, &ppid) || uid != our_uid)
			continue;
		snap_pid[snap_n] = pid;
		snap_ppid[snap_n] = ppid;
		snap_n++;
	}
	closedir(dir);
}

/* pid == anc, or anc is an ancestor of pid (live /proc walk — works for
 * stopped processes too, their status stays readable). */
static int
wsf_is_descendant(pid_t pid, pid_t anc)
{
	int depth;

	for (depth = 0; pid > 1 && depth < 32; depth++) {
		uid_t uid;
		pid_t ppid;
		if (pid == anc)
			return 1;
		if (!wsf_read_status(pid, &uid, &ppid))
			return 0;
		pid = ppid;
	}
	return 0;
}

/* HTPC visibility: the client's workspace vs the monitor's active one.
 * fs_ws NULL / no column = unbound → fall back to the tag test. */
static int
wsf_client_visible(Client *c)
{
	Monitor *m = c->mon ? c->mon : selmon;

	if (!m)
		return 1;
	if (m->active_ws) {
		Workspace *ws = NULL;
		if (c->isfullscreen && c->fs_ws)
			ws = c->fs_ws;
		else if (c->column && c->column->ws)
			ws = c->column->ws;
		if (ws)
			return ws == m->active_ws || c->issticky;
	}
	return VISIBLEON(c, m);
}

static int
wsf_outputs_dark(void)
{
	Monitor *m;

	wl_list_for_each(m, &mons, link) {
		if (m->is_virtual)
			continue;
		if (m->wlr_output && m->wlr_output->enabled && !m->asleep)
			return 0;
	}
	return 1;
}

static void
wsf_set_mute(int on)
{
	const char *argv[] = { "wpctl", "set-mute", "@DEFAULT_AUDIO_SINK@",
		on ? "1" : "0", NULL };
	spawn_cmd_async(argv);
}

/* Already covered by an existing frozen subtree? */
static int
wsf_frozen_covers(pid_t pid)
{
	int i;

	for (i = 0; i < ngroups; i++)
		if (wsf_is_descendant(pid, groups[i].leader))
			return 1;
	return 0;
}

static int
wsf_group_visible(const WsfGroup *g)
{
	Client *c;

	wl_list_for_each(c, &clients, link) {
		pid_t p;
		if (!wsf_client_visible(c))
			continue;
		p = client_get_pid(c);
		if (p > 1 && wsf_is_descendant(p, g->leader))
			return 1;
	}
	return 0;
}

static void
wsf_thaw_group_at(int i)
{
	int j;

	for (j = 0; j < groups[i].n; j++)
		kill(groups[i].pids[j], SIGCONT); /* ESRCH harmless */
	wlr_log(WLR_INFO, "wsfreeze: thawed group leader=%d (%d pids)",
		(int)groups[i].leader, groups[i].n);
	groups[i] = groups[--ngroups];
}

/* Freeze leader + every /proc-snapshot descendant.  Leader first, so a
 * stopped parent cannot fork new children mid-collection. */
static void
wsf_freeze_group(pid_t leader)
{
	static char member[WSF_SNAP_MAX];
	WsfGroup *g;
	pid_t self = getpid();
	int i, changed, found = 0;

	if (ngroups >= WSF_MAX_GROUPS)
		return;
	memset(member, 0, (size_t)snap_n);
	for (i = 0; i < snap_n; i++) {
		if (snap_pid[i] == leader) {
			member[i] = 1;
			found = 1;
		}
	}
	if (!found)
		return; /* leader exited between visibility check and now */
	do {
		changed = 0;
		for (i = 0; i < snap_n; i++) {
			int j;
			if (member[i])
				continue;
			for (j = 0; j < snap_n; j++) {
				if (member[j] && snap_ppid[i] == snap_pid[j]) {
					member[i] = 1;
					changed = 1;
					break;
				}
			}
		}
	} while (changed);

	g = &groups[ngroups];
	g->leader = leader;
	g->n = 0;
	if (kill(leader, SIGSTOP) == 0)
		g->pids[g->n++] = leader;
	for (i = 0; i < snap_n && g->n < WSF_GROUP_PIDS; i++) {
		if (!member[i] || snap_pid[i] == leader || snap_pid[i] == self)
			continue;
		if (kill(snap_pid[i], SIGSTOP) == 0)
			g->pids[g->n++] = snap_pid[i];
	}
	if (g->n) {
		ngroups++;
		wlr_log(WLR_INFO, "wsfreeze: froze group leader=%d (%d pids)",
			(int)leader, g->n);
	}
}

static void
wsf_maybe_freeze_client(Client *c, int force)
{
	Client *v;
	pid_t pid = client_get_pid(c);

	if (pid <= 1 || pid == getpid())
		return;
	if (!force && wsf_client_visible(c))
		return;
	if (wsf_frozen_covers(pid))
		return;
	if (!force) {
		/* A visible client living inside this tree (game launched
		 * from the hidden Steam window) protects the whole group. */
		wl_list_for_each(v, &clients, link) {
			pid_t p;
			if (!wsf_client_visible(v))
				continue;
			p = client_get_pid(v);
			if (p > 1 && wsf_is_descendant(p, pid))
				return;
		}
	}
	wsf_freeze_group(pid);
}

/* allow_freeze=0: thaw-only pass (synchronous, from wsfreeze_poke).
 * allow_freeze=1: full pass from the debounce timer. */
static void
wsf_update(int allow_freeze)
{
	Client *c;
	int i, dark;

	if (!htpc_mode_active) {
		if (session_dark) {
			session_dark = 0;
			wsf_set_mute(0);
		}
		while (ngroups)
			wsf_thaw_group_at(0);
		return;
	}

	dark = wsf_outputs_dark();

	if (!dark) {
		if (session_dark) {
			/* TV came back: wake everything, unmute. */
			session_dark = 0;
			wsf_set_mute(0);
			while (ngroups)
				wsf_thaw_group_at(0);
		} else {
			for (i = 0; i < ngroups; ) {
				int alive = kill(groups[i].leader, 0) == 0
					|| errno == EPERM;
				if (!alive || wsf_group_visible(&groups[i]))
					wsf_thaw_group_at(i);
				else
					i++;
			}
		}
	}

	if (!allow_freeze)
		return;

	if (dark) {
		if (!session_dark) {
			session_dark = 1;
			wsf_set_mute(1);
		}
		wsf_snapshot();
		wl_list_for_each(c, &clients, link)
			wsf_maybe_freeze_client(c, 1);
		return;
	}

	/* gamemode.c already freezes the background while a game runs;
	 * two owners SIGCONT-racing each other helps nobody. */
	if (game_mode_active)
		return;

	wsf_snapshot();
	wl_list_for_each(c, &clients, link)
		wsf_maybe_freeze_client(c, 0);
}

static int
wsf_timer_cb(void *data)
{
	(void)data;
	wsf_update(1);
	return 0;
}

void
wsfreeze_poke(void)
{
	wsf_update(0);
	if (!htpc_mode_active || !event_loop)
		return;
	if (!wsf_timer)
		wsf_timer = wl_event_loop_add_timer(event_loop, wsf_timer_cb,
			NULL);
	if (wsf_timer)
		wl_event_source_timer_update(wsf_timer, WSF_DELAY_MS);
}

/* Async-signal-safe: fatal-signal path and cleanup() both call this so a
 * crash never leaves the couch apps stopped. */
void
wsfreeze_thaw_all(void)
{
	int i, j;

	for (i = 0; i < ngroups; i++)
		for (j = 0; j < groups[i].n; j++)
			kill(groups[i].pids[j], SIGCONT);
	ngroups = 0;
	session_dark = 0;
}
