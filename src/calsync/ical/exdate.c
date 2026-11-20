/* EXDATE matching as libical compares. */
#include <stdlib.h>

#include "ics.h"

#define MARKS_MIN 16

/* Keys libical's comparison reduces to. */
typedef enum { MARK_DAY, MARK_INSTANT, MARK_FLOAT_WALL, MARK_ZONED_WALL } MarkKind;

struct Mark {
	MarkKind kind;
	int64_t key;
};

static int64_t
wall(struct icaltimetype t)
{
	return icaltime_as_timet_with_zone(t, NULL);
}

static int64_t
day_key(struct icaltimetype t)
{
	t.hour = t.minute = t.second = 0;
	return wall(t);
}

static int
mark_cmp(const void *pa, const void *pb)
{
	const struct Mark *a = pa, *b = pb;

	if (a->kind != b->kind)
		return a->kind < b->kind ? -1 : 1;
	return (a->key > b->key) - (a->key < b->key);
}

static void
mark(Exdates *x, MarkKind kind, int64_t key)
{
	if (x->n == x->cap) {
		x->cap = x->cap ? 2 * x->cap : MARKS_MIN;
		x->v = xrealloc(x->v, x->cap * sizeof *x->v);
	}
	x->v[x->n++] = (struct Mark){ kind, key };
}

static int
marked(const Exdates *x, MarkKind kind, int64_t key)
{
	struct Mark m = { kind, key };

	return !!bsearch(&m, x->v, x->n, sizeof m, mark_cmp);
}

/* Floating sides compare wall clocks. */
static void
mark_exdate(Exdates *x, struct icaltimetype t)
{
	if (t.is_date) {
		mark(x, MARK_DAY, day_key(t));
		return;
	}
	if (!t.zone) {
		mark(x, MARK_FLOAT_WALL, wall(t));
		return;
	}
	mark(x, MARK_INSTANT, icaltime_as_timet_with_zone(t, t.zone));
	mark(x, MARK_ZONED_WALL, wall(t));
}

/* Later years never hit window. */
int
exdates_load(Exdates *x, icalcomponent *c, int last_year)
{
	icalproperty *p = icalcomponent_get_first_property(c, ICAL_EXDATE_PROPERTY);
	struct icaltimetype t;
	int n = 0;

	for (; p; p = icalcomponent_get_next_property(c, ICAL_EXDATE_PROPERTY)) {
		if (++n > DATES_MAX)
			return -1;
		t = icalproperty_get_datetime_with_component(p, c);
		if (!icaltime_is_null_time(t) && t.year <= last_year)
			mark_exdate(x, t);
	}
	if (x->n)
		qsort(x->v, x->n, sizeof *x->v, mark_cmp);
	return 0;
}

int
exdates_hit(const Exdates *x, struct icaltimetype t)
{
	if (!x->n)
		return 0;
	if (t.is_date)
		return marked(x, MARK_DAY, day_key(t));
	if (marked(x, MARK_DAY, day_key(icaltime_convert_to_zone(t,
			icaltimezone_get_utc_timezone()))))
		return 1;
	if (t.zone)
		return marked(x, MARK_INSTANT, icaltime_as_timet_with_zone(t, t.zone))
			|| marked(x, MARK_FLOAT_WALL, wall(t));
	return marked(x, MARK_FLOAT_WALL, wall(t)) || marked(x, MARK_ZONED_WALL, wall(t));
}
