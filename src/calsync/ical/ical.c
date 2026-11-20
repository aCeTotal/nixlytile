/* iCalendar expansion into events. */
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ics.h"

#define ZONE_SLACK_S  86400
#define OVERRIDES_MIN 16
#define EVENTS_MAX    50000
#define EXPAND_CPU_NS 5000000000LL
#define NO_TITLE      "(No title)"

typedef enum { CLOCK_ZONED, CLOCK_FLOATING, CLOCK_DATE } Clock;

typedef struct {
	const char *uid;
	time_t at;
} Override;

typedef struct {
	const CalMeta *meta;
	const SyncWindow *w;
	EventList *out;
	Recur rec;
	Override *ov;
	size_t nov, cap;
	const char *uid;
	Clock clock;
	const icaltimezone *zone;
	CalEvent proto;
	int aborted;
} Expand;

/* Floating times count as UTC. */
static time_t
span_time(struct icaltimetype t)
{
	return icaltime_as_timet_with_zone(t,
			t.zone ? t.zone : icaltimezone_get_utc_timezone());
}

/* UTC wall clock as local. */
static int64_t
as_local(time_t t)
{
	struct tm tm;

	gmtime_r(&t, &tm);
	tm.tm_isdst = -1;
	return mktime(&tm);
}

static int
override_cmp(const void *pa, const void *pb)
{
	const Override *a = pa, *b = pb;

	if (a->at != b->at)
		return a->at < b->at ? -1 : 1;
	return strcmp(a->uid, b->uid);
}

static int
in_window(const CalEvent *e, const SyncWindow *w)
{
	if (e->start == e->end)
		return e->start >= w->full_from && e->start < w->full_to;
	return e->end > w->full_from && e->start < w->full_to;
}

static void
place_days(CalEvent *e, const struct icaltime_span *s, const icaltimezone *z)
{
	struct icaltimetype a = icaltime_from_timet_with_zone(s->start, 1, z);
	struct icaltimetype b = icaltime_from_timet_with_zone(s->end, 1, z);

	e->flags = EV_ALLDAY;
	e->start = local_midnight(a.year, a.month, a.day);
	e->end = local_midnight(b.year, b.month, b.day);
	if (e->end <= e->start)
		e->end = local_midnight(a.year, a.month, a.day + 1);
}

static void
place(CalEvent *e, const struct icaltime_span *s, const Expand *x)
{
	switch (x->clock) {
	case CLOCK_DATE:
		place_days(e, s, x->zone);
		return;
	case CLOCK_FLOATING:
		e->start = as_local(s->start);
		e->end = as_local(s->end);
		return;
	case CLOCK_ZONED:
		e->start = s->start;
		e->end = s->end;
		return;
	}
}

/* Full lists abort the calendar. */
static int
on_instance(const icaltime_span *span, void *arg)
{
	Expand *x = arg;
	Override key = { x->uid, span->start };

	if (x->uid && x->nov
			&& bsearch(&key, x->ov, x->nov, sizeof *x->ov, override_cmp))
		return 0;
	place(&x->proto, span, x);
	if (x->proto.end < x->proto.start)
		x->proto.end = x->proto.start;
	if (!in_window(&x->proto, x->w))
		return 0;
	if (x->out->n >= EVENTS_MAX)
		return -1;
	memcpy(events_push(x->out), &x->proto, sizeof x->proto);
	return 0;
}

static void
describe(CalEvent *e, icalcomponent *c, const CalMeta *m)
{
	memset(e, 0, sizeof *e);
	event_text(e->title, sizeof e->title, icalcomponent_get_summary(c));
	if (!e->title[0])
		memcpy(e->title, NO_TITLE, sizeof NO_TITLE);
	event_text(e->where, sizeof e->where, icalcomponent_get_location(c));
	event_text(e->cal, sizeof e->cal, m->name);
	e->color = m->color;
}

/* Only masters skip overridden instances. */
static void
expand_event(icalcomponent *c, Expand *x)
{
	struct icaltimetype start = icalcomponent_get_dtstart(c);
	int moved = !!icalcomponent_get_first_property(c, ICAL_RECURRENCEID_PROPERTY);
	size_t kept = x->out->n;
	RecurResult rc;

	if (icaltime_is_null_time(start)
			|| icalcomponent_get_status(c) == ICAL_STATUS_CANCELLED)
		return;
	describe(&x->proto, c, x->meta);
	x->clock = start.is_date ? CLOCK_DATE
			: start.zone ? CLOCK_ZONED : CLOCK_FLOATING;
	x->zone = start.zone ? start.zone : icaltimezone_get_utc_timezone();
	x->uid = moved ? NULL : icalcomponent_get_uid(c);
	rc = recur_each(c, &x->rec);
	if (rc == RECUR_DROP)
		x->out->n = kept;
	x->aborted |= rc == RECUR_ABORT;
}

static int
finished(icalcomponent *c)
{
	icalproperty_status st = icalcomponent_get_status(c);
	icalproperty *pc = icalcomponent_get_first_property(c,
			ICAL_PERCENTCOMPLETE_PROPERTY);

	return st == ICAL_STATUS_COMPLETED || st == ICAL_STATUS_CANCELLED
		|| icalcomponent_get_first_property(c, ICAL_COMPLETED_PROPERTY)
		|| (pc && icalproperty_get_percentcomplete(pc) >= PERCENT_DONE);
}

static void
add_todo(icalcomponent *c, Expand *x)
{
	struct icaltimetype due = icalcomponent_get_due(c);
	CalEvent *e;

	if (finished(c))
		return;
	if (x->out->n >= EVENTS_MAX) {
		x->aborted = 1;
		return;
	}
	if (icaltime_is_null_time(due))
		due = icalcomponent_get_dtstart(c);
	e = events_push(x->out);
	describe(e, c, x->meta);
	e->flags = EV_TASK;
	if (icaltime_is_null_time(due))
		return;
	if (!due.is_date) {
		e->start = e->end = due.zone ? span_time(due) : as_local(span_time(due));
		return;
	}
	e->flags |= EV_ALLDAY;
	e->start = local_midnight(due.year, due.month, due.day);
	e->end = local_midnight(due.year, due.month, due.day + 1);
}

static void
note_override(icalcomponent *c, Expand *x)
{
	icalproperty *rid = icalcomponent_isa(c) == ICAL_VEVENT_COMPONENT ?
		icalcomponent_get_first_property(c, ICAL_RECURRENCEID_PROPERTY) : NULL;
	const char *uid;

	if (!rid)
		return;
	if (x->nov == x->cap) {
		x->cap = x->cap ? 2 * x->cap : OVERRIDES_MIN;
		x->ov = xrealloc(x->ov, x->cap * sizeof *x->ov);
	}
	uid = icalcomponent_get_uid(c);
	x->ov[x->nov++] = (Override){ uid ? uid : "",
			span_time(icalproperty_get_datetime_with_component(rid, c)) };
}

static void
expand_one(icalcomponent *c, Expand *x)
{
	if (icalcomponent_isa(c) == ICAL_VEVENT_COMPONENT)
		expand_event(c, x);
	if (icalcomponent_isa(c) == ICAL_VTODO_COMPONENT)
		add_todo(c, x);
}

/* Spent CPU budget stops walking. */
static void
walk(icalcomponent *cal, void (*visit)(icalcomponent *, Expand *), Expand *x)
{
	icalcompiter it = icalcomponent_begin_component(cal, ICAL_ANY_COMPONENT);
	icalcomponent *c;

	for (; !x->aborted && (c = icalcompiter_deref(&it)); icalcompiter_next(&it)) {
		visit(c, x);
		x->aborted |= recur_clock() > x->rec.deadline;
	}
}

/* Budget overrun drops every event. */
int
ical_expand(const char *text, const CalMeta *m, const SyncWindow *w,
		EventList *out)
{
	icalcomponent *cal = ics_parse(text);
	Expand x = { .meta = m, .w = w, .out = out };
	size_t first = out->n;

	if (!cal)
		return -1;
	if (icalcomponent_isa(cal) != ICAL_VCALENDAR_COMPONENT) {
		icalcomponent_free(cal);
		return -1;
	}
	ics_vet_zones(cal);
	x.rec = (Recur){
		.limit = { w->full_from - ZONE_SLACK_S, w->full_to + ZONE_SLACK_S, 1 },
		.deadline = recur_clock() + EXPAND_CPU_NS,
		.emit = on_instance,
		.ctx = &x,
	};
	walk(cal, note_override, &x);
	if (x.nov)
		qsort(x.ov, x.nov, sizeof *x.ov, override_cmp);
	walk(cal, expand_one, &x);
	free(x.ov);
	icalcomponent_free(cal);
	if (x.aborted) {
		out->n = first;
		return -1;
	}
	return (int)(out->n - first);
}
