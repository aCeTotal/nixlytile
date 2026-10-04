#include "nixlytile.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "client.h"
#include "procsnap.h"
#include "pwaudible.h"

#define REAP_MAX_ROOTS  64
#define REAP_GRACE_MS   10000
#define REAP_KILL_MS    5000
#define REAP_TICK_MS    2000
#define REAP_RECHECK_MS 30000
#define EXE_SUFFIX      ".exe"

typedef enum { ROOT_WATCH, ROOT_TERMINATED } RootPhase;

/* A window owner pid. */
typedef struct {
	pid_t pid;
	unsigned long long start;
	uint64_t lost_ms;
	uint64_t term_ms;
	RootPhase phase;
} Root;

typedef enum { VERDICT_KEEP, VERDICT_WAIT, VERDICT_SPARE, VERDICT_END } Verdict;

static const char *const voice_comms[] = {
	"discord", "vesktop", "webcord", "legcord", NULL
};

/* X11 apps share these pids. */
static const char *const display_comms[] = {
	"xwayland-satell", "Xwayland", NULL
};

static Root roots[REAP_MAX_ROOTS];
static int nroots;

static struct wl_event_source *tick;

static void reap_pass(void);

static int
comm_in(const char *comm, const char *const list[])
{
	int i;

	for (i = 0; list[i]; i++) {
		if (!strcasecmp(comm, list[i]))
			return 1;
	}
	return 0;
}

static int
is_voice(const char *comm)
{
	int i;

	for (i = 0; voice_comms[i]; i++) {
		if (strcasestr(comm, voice_comms[i]))
			return 1;
	}
	return 0;
}

static int
is_game(const char *comm)
{
	size_t len = strlen(comm), slen = strlen(EXE_SUFFIX);

	if (len > slen && !strcasecmp(comm + len - slen, EXE_SUFFIX))
		return 1;
	return comm_in(comm, game_runtime_comms);
}

static int
has_window(pid_t root)
{
	Client *c;

	wl_list_for_each(c, &clients, link) {
		if (procsnap_in_tree(procsnap_find(client_get_pid(c)), root))
			return 1;
	}
	return 0;
}

static int
is_spared(pid_t root)
{
	int i;

	for (i = 0; i < procsnap_n; i++) {
		const Proc *p = &procsnap[i];

		if (!procsnap_in_tree(i, root))
			continue;
		if (p->tty || is_game(p->comm) || pwaudible_has(p->pid))
			return 1;
	}
	return 0;
}

static void
signal_tree(pid_t root, int sig)
{
	pid_t self = getpid();
	int i;

	for (i = 0; i < procsnap_n; i++) {
		if (procsnap[i].pid == self || !procsnap_in_tree(i, root))
			continue;
		kill(procsnap[i].pid, SIGCONT);
		kill(procsnap[i].pid, sig);
	}
}

static Verdict
judge(Root *r, uint64_t now)
{
	if (has_window(r->pid)) {
		r->lost_ms = 0;
		r->phase = ROOT_WATCH;
		return VERDICT_KEEP;
	}
	if (r->phase == ROOT_TERMINATED)
		return now - r->term_ms >= REAP_KILL_MS ? VERDICT_END : VERDICT_WAIT;
	if (!r->lost_ms)
		r->lost_ms = now;
	if (now - r->lost_ms < REAP_GRACE_MS)
		return VERDICT_WAIT;
	if (now - pwaudible_at() > REAP_TICK_MS) {
		pwaudible_query(reap_pass);
		return VERDICT_SPARE;
	}
	return is_spared(r->pid) ? VERDICT_SPARE : VERDICT_END;
}

/* Next delay; 0 drops the root. */
static uint64_t
act(Root *r, Verdict v, uint64_t now)
{
	if (v == VERDICT_WAIT)
		return REAP_TICK_MS;
	if (v == VERDICT_SPARE)
		return REAP_RECHECK_MS;
	if (r->phase == ROOT_TERMINATED) {
		wlr_log(WLR_INFO, "appreap: killing pid %d", (int)r->pid);
		signal_tree(r->pid, SIGKILL);
		return 0;
	}
	wlr_log(WLR_INFO, "appreap: ending windowless pid %d", (int)r->pid);
	signal_tree(r->pid, SIGTERM);
	r->phase = ROOT_TERMINATED;
	r->term_ms = now;
	return REAP_KILL_MS;
}

static int
root_alive(const Root *r)
{
	int idx = procsnap_find(r->pid);

	return idx >= 0 && procsnap[idx].start == r->start;
}

static void
reap_pass(void)
{
	uint64_t now = monotonic_msec(), next = 0;
	int i = 0;

	procsnap_take();
	while (i < nroots) {
		Root *r = &roots[i];
		Verdict v;
		uint64_t delay;

		if (!root_alive(r)) {
			roots[i] = roots[--nroots];
			continue;
		}
		v = judge(r, now);
		delay = v == VERDICT_KEEP ? UINT64_MAX : act(r, v, now);
		if (!delay) {
			roots[i] = roots[--nroots];
			continue;
		}
		if (delay != UINT64_MAX && (!next || delay < next))
			next = delay;
		i++;
	}
	if (tick)
		wl_event_source_timer_update(tick, (int)next);
}

static int
tick_cb(void *data)
{
	reap_pass();
	return 0;
}

/* Systemd owns its services. */
static int
is_service(pid_t pid)
{
	char path[32], cg[512];
	size_t n;
	FILE *f;

	snprintf(path, sizeof(path), "/proc/%d/cgroup", pid);
	f = fopen(path, "r");
	if (!f)
		return 1;
	n = fread(cg, 1, sizeof(cg) - 1, f);
	fclose(f);
	cg[n] = '\0';
	return strstr(cg, ".service") != NULL;
}

void
appreap_track(Client *c)
{
	pid_t pid = client_get_pid(c);
	Proc p;
	int i;

	if (pid <= 1 || pid == getpid() || !procsnap_read(pid, &p))
		return;
	for (i = 0; i < nroots; i++) {
		if (roots[i].pid == pid && roots[i].start == p.start)
			return;
	}
	if (nroots >= REAP_MAX_ROOTS || comm_in(p.comm, display_comms))
		return;
	if (is_voice(p.comm) || is_service(pid))
		return;
	roots[nroots++] = (Root){ .pid = pid, .start = p.start };
}

void
appreap_poke(void)
{
	if (!nroots || !event_loop)
		return;
	if (!tick)
		tick = wl_event_loop_add_timer(event_loop, tick_cb, NULL);
	if (tick)
		wl_event_source_timer_update(tick, REAP_TICK_MS);
}
