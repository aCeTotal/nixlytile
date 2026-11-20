/* Timezone definitions libical expands safely. */
#include <stdlib.h>

#include "ics.h"

#define ZONE_YEARS_MAX 32768
#define ZONE_LAST_YEAR 2582
#define MONTHS         12
#define MONTHDAYS_MAX  7
#define NTH_MAX        5
#define ANY_LEAP_YEAR  2000
#define BY_COUNT(a)    by_count(a, sizeof (a) / sizeof *(a))

static int
by_count(const short *by, size_t size)
{
	size_t n = 0;

	while (n < size && by[n] != ICAL_RECURRENCE_ARRAY_MAX)
		n++;
	return (int)n;
}

static int
monthdays_ok(const struct icalrecurrencetype *r, int n)
{
	int last = icaltime_days_in_month(r->by_month[0], ANY_LEAP_YEAR);

	for (int i = 0; i < n; i++)
		if (!r->by_month_day[i] || abs(r->by_month_day[i]) > last)
			return 0;
	return 1;
}

/* Yearly dates that always recur. */
static int
zone_rule_ok(const struct icalrecurrencetype *r)
{
	int months = BY_COUNT(r->by_month), days = BY_COUNT(r->by_day);
	int mdays = BY_COUNT(r->by_month_day);
	int nth = days ? icalrecurrencetype_day_position(r->by_day[0]) : 0;

	if (r->freq != ICAL_YEARLY_RECURRENCE || r->interval != 1 || r->rscale
			|| BY_COUNT(r->by_second) || BY_COUNT(r->by_minute)
			|| BY_COUNT(r->by_hour) || BY_COUNT(r->by_year_day)
			|| BY_COUNT(r->by_week_no) || BY_COUNT(r->by_set_pos))
		return 0;
	if (!months)
		return !days && !mdays;
	return months == 1 && r->by_month[0] >= 1 && r->by_month[0] <= MONTHS
		&& days <= 1 && abs(nth) <= NTH_MAX && !(nth && mdays)
		&& mdays <= MONTHDAYS_MAX && monthdays_ok(r, mdays);
}

/* Years libical walks from start. */
static long
rule_years(int from)
{
	if (from > ZONE_LAST_YEAR)
		return 1;
	return ZONE_LAST_YEAR + 1 - (from < 0 ? 0 : from);
}

/* DTSTART first: lookups reset iteration. */
static long
observance_cost(icalcomponent *obs)
{
	long per_rule = rule_years(icalcomponent_get_dtstart(obs).year), years = 0;
	icalproperty *p = icalcomponent_get_first_property(obs, ICAL_RRULE_PROPERTY);
	struct icalrecurrencetype r;

	for (; p; p = icalcomponent_get_next_property(obs, ICAL_RRULE_PROPERTY)) {
		r = icalproperty_get_rrule(p);
		if (!zone_rule_ok(&r))
			return -1;
		years += per_rule;
	}
	return years;
}

/* Rule-years to expand, or -1. */
static long
zone_cost(icalcomponent *zone)
{
	icalcomponent *obs = icalcomponent_get_first_component(zone, ICAL_ANY_COMPONENT);
	long years = 0, cost;

	for (; obs; obs = icalcomponent_get_next_component(zone, ICAL_ANY_COMPONENT)) {
		cost = observance_cost(obs);
		if (cost < 0 || cost > ZONE_YEARS_MAX - years)
			return -1;
		years += cost;
	}
	return years;
}

/* Costly zones yield to builtins. */
static void
vet_zones(icalcomponent *parent, long *budget)
{
	icalcomponent *c = icalcomponent_get_first_component(parent, ICAL_ANY_COMPONENT);
	icalcomponent *next;
	long cost;

	for (; c; c = next) {
		next = icalcomponent_get_next_component(parent, ICAL_ANY_COMPONENT);
		if (icalcomponent_isa(c) != ICAL_VTIMEZONE_COMPONENT) {
			vet_zones(c, budget);
			continue;
		}
		cost = zone_cost(c);
		if (cost >= 0 && cost <= *budget) {
			*budget -= cost;
			continue;
		}
		icalcomponent_remove_component(parent, c);
		icalcomponent_free(c);
	}
}

void
ics_vet_zones(icalcomponent *cal)
{
	long budget = ZONE_YEARS_MAX;

	vet_zones(cal, &budget);
}
