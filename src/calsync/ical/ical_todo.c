/* VTODO building and completion. */
#include "ics.h"

#define PRODID      "-//nixlytile//calsync//EN"
#define ICS_VERSION "2.0"

char *
ical_todo_new(const TodoNew *t, const char *uid)
{
	struct icaltimetype now = icaltime_current_time_with_zone(
			icaltimezone_get_utc_timezone());
	struct icaltimetype due = { .year = t->y, .month = t->m, .day = t->d,
			.is_date = 1 };
	icalcomponent *todo = icalcomponent_vanew(ICAL_VTODO_COMPONENT,
			icalproperty_new_uid(uid), icalproperty_new_dtstamp(now),
			icalproperty_new_created(now), icalproperty_new_summary(t->title),
			icalproperty_new_status(ICAL_STATUS_NEEDSACTION), NULL);
	icalcomponent *cal;
	char *ics;

	if (t->y)
		icalcomponent_add_property(todo, icalproperty_new_due(due));
	cal = icalcomponent_vanew(ICAL_VCALENDAR_COMPONENT,
			icalproperty_new_prodid(PRODID),
			icalproperty_new_version(ICS_VERSION), todo, NULL);
	ics = icalcomponent_as_ical_string_r(cal);
	icalcomponent_free(cal);
	return ics;
}

/* Replace same-kind properties with p. */
static void
put(icalcomponent *c, icalproperty *p)
{
	icalproperty_kind kind = icalproperty_isa(p);
	icalproperty *old;

	while ((old = icalcomponent_get_first_property(c, kind))) {
		icalcomponent_remove_property(c, old);
		icalproperty_free(old);
	}
	icalcomponent_add_property(c, p);
}

static void
mark_done(icalcomponent *todo, int64_t now)
{
	struct icaltimetype at = icaltime_from_timet_with_zone((time_t)now, 0,
			icaltimezone_get_utc_timezone());
	icalproperty *done[] = {
		icalproperty_new_status(ICAL_STATUS_COMPLETED),
		icalproperty_new_completed(at),
		icalproperty_new_percentcomplete(PERCENT_DONE),
		icalproperty_new_dtstamp(at),
		icalproperty_new_lastmodified(at),
	};

	for (size_t i = 0; i < sizeof done / sizeof *done; i++)
		put(todo, done[i]);
}

char *
ical_todo_complete(const char *text, int64_t now)
{
	icalcomponent *cal = ics_parse(text);
	icalcomponent *todo;
	char *ics = NULL;

	if (!cal)
		return NULL;
	todo = icalcomponent_get_first_component(cal, ICAL_VTODO_COMPONENT);
	if (todo) {
		mark_done(todo, now);
		ics = icalcomponent_as_ical_string_r(cal);
	}
	icalcomponent_free(cal);
	return ics;
}
