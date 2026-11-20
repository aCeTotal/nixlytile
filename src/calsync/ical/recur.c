/* Recurrence expansion under hard budgets. */
#include <stdlib.h>
#include <time.h>

#include "ics.h"

#define STEPS_MAX      50000
#define NS_PER_S       1000000000
#define SECS_PER_MIN   60
#define SECS_PER_HOUR  3600
#define SECS_PER_DAY   86400
#define HOURS_PER_DAY  24
#define DAYS_PER_WEEK  7
#define MONTH_MIN_DAYS 28
#define YEAR_MIN_DAYS  365
#define EDGE_DAYS      3
#define INSTANCES_MAX  ((FULL_BACK_DAYS + FULL_AHEAD_DAYS + EDGE_DAYS) * HOURS_PER_DAY)
#define LENGTH(a)      (sizeof (a) / sizeof *(a))

typedef struct {
	const Recur *r;
	icalcomponent *c;
	struct icaltimetype dtstart, end;
	int last_year;
	icaltime_span base, limit;
	time_t duration;
	Exdates ex;
	int64_t steps;
	int emitted;
} Series;

static const int64_t freq_seconds[] = {
	[ICAL_SECONDLY_RECURRENCE] = 1,
	[ICAL_MINUTELY_RECURRENCE] = SECS_PER_MIN,
	[ICAL_HOURLY_RECURRENCE] = SECS_PER_HOUR,
	[ICAL_DAILY_RECURRENCE] = SECS_PER_DAY,
	[ICAL_WEEKLY_RECURRENCE] = DAYS_PER_WEEK * SECS_PER_DAY,
	[ICAL_MONTHLY_RECURRENCE] = MONTH_MIN_DAYS * SECS_PER_DAY,
	[ICAL_YEARLY_RECURRENCE] = YEAR_MIN_DAYS * SECS_PER_DAY,
};

int64_t
recur_clock(void)
{
	struct timespec t;

	clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t);
	return (int64_t)t.tv_sec * NS_PER_S + t.tv_nsec;
}

static int
charge(Series *s, int64_t steps)
{
	if (steps > s->steps)
		return 0;
	s->steps -= steps;
	return 1;
}

static RecurResult
offer(Series *s, struct icaltimetype at, icaltime_span *span)
{
	if (!icaltime_span_overlaps(span, &s->limit) || exdates_hit(&s->ex, at))
		return RECUR_OK;
	if (++s->emitted > INSTANCES_MAX)
		return RECUR_DROP;
	return s->r->emit(span, s->r->ctx) < 0 ? RECUR_ABORT : RECUR_OK;
}

/* BY parts can skip steps. */
static int
filtered(const struct icalrecurrencetype *r)
{
	const short *by[] = { r->by_second, r->by_minute, r->by_hour, r->by_day,
		r->by_month_day, r->by_year_day, r->by_week_no, r->by_month, r->by_set_pos };

	for (size_t i = 0; i < LENGTH(by); i++)
		if (by[i][0] != ICAL_RECURRENCE_ARRAY_MAX)
			return 1;
	return 0;
}

/* Iterator steps until window end. */
static int64_t
span_steps(const Series *s, const struct icalrecurrencetype *rule)
{
	time_t first = s->base.start;
	int64_t steps = 1;

	if (!rule->count && first < s->limit.start - s->duration)
		first = s->limit.start - s->duration;
	if (s->limit.end > first)
		steps += (s->limit.end - first) / (freq_seconds[rule->freq] * rule->interval);
	if (rule->count && rule->count < steps && !filtered(rule))
		return rule->count;
	return steps;
}

/* Stop libical at window end. */
static struct icalrecurrencetype
clamped(struct icalrecurrencetype rule, const Series *s)
{
	struct icaltimetype end = icaltime_convert_to_zone(s->end,
			(icaltimezone *)s->dtstart.zone);

	if (icaltime_is_null_time(rule.until) || icaltime_compare(rule.until, end) > 0)
		rule.until = end;
	return rule;
}

static RecurResult
occurrence(Series *s, struct icaltimetype t)
{
	icaltime_span span = { .is_busy = 1 };

	if (!charge(s, 1))
		return RECUR_DROP;
	span.start = icaltime_as_timet_with_zone(t,
			t.zone ? t.zone : icaltimezone_get_utc_timezone());
	span.end = span.start + s->duration;
	return offer(s, t, &span);
}

static RecurResult
rule_each(Series *s, struct icalrecurrencetype rule)
{
	icalrecur_iterator *it;
	struct icaltimetype t;
	RecurResult rc = RECUR_OK;

	if ((size_t)rule.freq >= LENGTH(freq_seconds))
		return RECUR_OK;
	if (!charge(s, span_steps(s, &rule)))
		return RECUR_DROP;
	if (recur_clock() > s->r->deadline)
		return RECUR_ABORT;
	it = icalrecur_iterator_new(clamped(rule, s), s->dtstart);
	if (!it)
		return RECUR_OK;
	if (!rule.count)
		icalrecur_iterator_set_start(it, icaltime_from_timet_with_zone(
				s->limit.start - s->duration, 0, icaltimezone_get_utc_timezone()));
	for (t = icalrecur_iterator_next(it); rc == RECUR_OK && !icaltime_is_null_time(t);
			t = icalrecur_iterator_next(it))
		rc = occurrence(s, t);
	icalrecur_iterator_free(it);
	return rc;
}

/* Like libical: periods in UTC. */
static RecurResult
rdate(Series *s, struct icaldatetimeperiodtype rp)
{
	const icaltimezone *zone = rp.time.zone ? rp.time.zone
			: icaltimezone_get_utc_timezone();
	struct icaltimetype at = icaltime_is_null_time(rp.time) ? rp.period.start : rp.time;
	icaltime_span span = { .is_busy = 1 };

	if (icaltime_is_null_time(at) || at.year > s->last_year)
		return RECUR_OK;
	span.start = icaltime_as_timet_with_zone(at, zone);
	span.end = span.start + s->duration;
	if (icaltime_is_null_time(rp.time) && icaltime_is_null_time(rp.period.end))
		span.end = span.start + icaldurationtype_as_int(rp.period.duration);
	if (icaltime_is_null_time(rp.time) && !icaltime_is_null_time(rp.period.end))
		span.end = icaltime_as_timet_with_zone(rp.period.end, zone);
	return offer(s, at, &span);
}

static RecurResult
rdates(Series *s)
{
	icalproperty *p = icalcomponent_get_first_property(s->c, ICAL_RDATE_PROPERTY);
	RecurResult rc = RECUR_OK;
	int n = 0;

	for (; rc == RECUR_OK && p;
			p = icalcomponent_get_next_property(s->c, ICAL_RDATE_PROPERTY))
		rc = ++n > DATES_MAX ? RECUR_DROP : rdate(s, icalproperty_get_rdate(p));
	return rc;
}

/* Mirrors icalcomponent_foreach_recurrence. */
static RecurResult
expand(Series *s)
{
	icalproperty *p;
	RecurResult rc = RECUR_OK;

	if (exdates_load(&s->ex, s->c, s->last_year))
		return RECUR_DROP;
	p = icalcomponent_get_first_property(s->c, ICAL_RRULE_PROPERTY);
	if (!p)
		rc = offer(s, s->dtstart, &s->base);
	for (; rc == RECUR_OK && p;
			p = icalcomponent_get_next_property(s->c, ICAL_RRULE_PROPERTY))
		rc = rule_each(s, icalproperty_get_rrule(p));
	return rc == RECUR_OK ? rdates(s) : rc;
}

RecurResult
recur_each(icalcomponent *c, const Recur *r)
{
	Series s = { .r = r, .c = c, .limit = r->limit, .steps = STEPS_MAX };
	RecurResult rc;

	s.end = icaltime_from_timet_with_zone(r->limit.end, 0,
			icaltimezone_get_utc_timezone());
	s.last_year = s.end.year + 1;
	s.dtstart = icalcomponent_get_dtstart(c);
	s.base = icaltime_span_new(s.dtstart, icalcomponent_get_dtend(c), 1);
	s.duration = s.base.end - s.base.start;
	rc = expand(&s);
	free(s.ex.v);
	return rc;
}
