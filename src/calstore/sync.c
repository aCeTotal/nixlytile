/* Minute sync and account status. */
#include "nixlytile.h"
#include "calstore/calstore.h"

#define SYNC_EVERY_MS 60000
#define FIRST_SYNC_MS 4000
#define STALE_S       30
#define SPIN_MS       80

static struct wl_event_source *minute_timer, *spin_timer;
static int64_t started_at, ok_at;
static char sync_error[ERROR_LEN];
static AccountState states[ACCOUNTS_MAX];
static int nstates;

/* Reuses slots of removed accounts. */
static AccountState *
state_for(const char *id)
{
	AccountState *s = NULL;

	for (int i = 0; i < nstates; i++)
		if (!strcmp(states[i].id, id))
			return &states[i];
	for (int i = 0; i < nstates && !s; i++)
		if (!store_account_by_id(states[i].id))
			s = &states[i];
	if (!s && nstates < ACCOUNTS_MAX)
		s = &states[nstates++];
	if (!s)
		return NULL;
	memset(s, 0, sizeof(*s));
	snprintf(s->id, sizeof(s->id), "%s", id);
	return s;
}

/* "<id>\tok" or "<id>\terror\t<msg>". */
void
sync_account_line(char *args)
{
	char *status = strchr(args, '\t'), *msg;
	AccountState *s;

	if (!status)
		return;
	*status++ = '\0';
	msg = strchr(status, '\t');
	if (!(s = state_for(args)))
		return;
	if (!strcmp(status, "ok")) {
		s->ok_at = (int64_t)time(NULL);
		s->error[0] = '\0';
		return;
	}
	snprintf(s->error, sizeof(s->error), "%s", msg ? msg + 1 : "Failed");
}

void
sync_finished(const char *error)
{
	snprintf(sync_error, sizeof(sync_error), "%s", error);
	if (!error[0])
		ok_at = (int64_t)time(NULL);
}

void
sync_start(void)
{
	if (job_busy(JOB_SYNC))
		return;
	started_at = (int64_t)time(NULL);
	job_spawn(&(JobSpec){ .kind = JOB_SYNC, .cmd = "sync" });
}

void
sync_maybe(void)
{
	if ((int64_t)time(NULL) - started_at >= STALE_S)
		sync_start();
}

static int
minute_tick(void *data)
{
	sync_start();
	wl_event_source_timer_update(minute_timer, SYNC_EVERY_MS);
	return 0;
}

static int
spin_tick(void *data)
{
	if (!job_busy(JOB_ADD))
		return 0;
	calendar_tick();
	wl_event_source_timer_update(spin_timer, SPIN_MS);
	return 0;
}

void
spin_start(void)
{
	wl_event_source_timer_update(spin_timer, SPIN_MS);
}

void
sync_init(void)
{
	store_load();
	remind_init();
	minute_timer = wl_event_loop_add_timer(event_loop, minute_tick, NULL);
	spin_timer = wl_event_loop_add_timer(event_loop, spin_tick, NULL);
	wl_event_source_timer_update(minute_timer, FIRST_SYNC_MS);
}

int
sync_running(void)
{
	return job_busy(JOB_SYNC);
}

int64_t
sync_last_ok(void)
{
	return ok_at;
}

const char *
sync_last_error(void)
{
	return sync_error;
}

const AccountState *
account_state(const char *id)
{
	for (int i = 0; i < nstates; i++)
		if (!strcmp(states[i].id, id))
			return &states[i];
	return NULL;
}
