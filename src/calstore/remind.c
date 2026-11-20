/* Reminders before timed events. */
#include "nixlytile.h"
#include "calstore/calstore.h"

#define HOLD_MS     15000
#define RECHECK_MS  (15 * 60 * 1000)
#define BODY_LEN    192

static const int leads_min[] = { 30, 10 };

static struct wl_event_source *timer;
static int64_t done_until;

static int
timed_event(const CalItem *it)
{
	return !(it->flags & (CAL_ALLDAY | CAL_TASK));
}

/* Earliest reminder after t for one lead. */
static int64_t
next_for(int64_t t, int lead_s)
{
	for (size_t i = store_after(t + lead_s); i < store_count(); i++)
		if (timed_event(store_item(i)))
			return store_item(i)->start - lead_s;
	return INT64_MAX;
}

static void
notify(const CalItem *it, int64_t now)
{
	time_t start = (time_t)it->start;
	const char *where = store_str(it->where);
	char at[16], body[BODY_LEN];
	struct tm tm;

	localtime_r(&start, &tm);
	strftime(at, sizeof(at), "%H:%M", &tm);
	snprintf(body, sizeof(body), "Starts in %lld min · %s%s%s",
			(long long)((it->start - now + MIN_S - 1) / MIN_S), at,
			where[0] ? " · " : "", where);
	notifyd_post(&(NotifyPost){ "Calendar", store_str(it->title), body,
			HOLD_MS });
}

/* Due in (done_until, now], not started. */
static void
fire_lead(int64_t now, int lead_s)
{
	for (size_t i = store_after(done_until + lead_s); i < store_count(); i++) {
		const CalItem *it = store_item(i);

		if (it->start > now + lead_s)
			return;
		if (timed_event(it) && it->start > now)
			notify(it, now);
	}
}

static int
tick(void *data)
{
	int64_t now = (int64_t)time(NULL);

	for (size_t i = 0; i < LENGTH(leads_min); i++)
		fire_lead(now, leads_min[i] * MIN_S);
	done_until = now;
	remind_arm();
	return 0;
}

/* Sleeps until the next reminder. */
void
remind_arm(void)
{
	int64_t now = (int64_t)time(NULL), next = INT64_MAX, wait_ms;

	if (!timer)
		return;
	for (size_t i = 0; i < LENGTH(leads_min); i++)
		next = MIN(next, next_for(done_until, leads_min[i] * MIN_S));
	wait_ms = next == INT64_MAX ? RECHECK_MS :
		MIN((next - now) * 1000, RECHECK_MS);
	wl_event_source_timer_update(timer, (int)MAX(wait_ms, 1));
}

void
remind_init(void)
{
	done_until = (int64_t)time(NULL);
	timer = wl_event_loop_add_timer(event_loop, tick, NULL);
	remind_arm();
}
