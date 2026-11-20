/* Untrusted iCalendar parsing internals. */
#ifndef ICS_H
#define ICS_H

#include <libical/ical.h>

#include "calsync.h"

#define DATES_MAX 4096

typedef enum { RECUR_OK, RECUR_DROP, RECUR_ABORT } RecurResult;

/* Emit below zero aborts. */
typedef struct {
	icaltime_span limit;
	int64_t deadline;
	int (*emit)(const icaltime_span *span, void *ctx);
	void *ctx;
} Recur;

typedef struct {
	struct Mark *v;
	size_t n, cap;
} Exdates;

icalcomponent *ics_parse(const char *text);
void ics_vet_zones(icalcomponent *cal);

int exdates_load(Exdates *x, icalcomponent *c, int last_year);
int exdates_hit(const Exdates *x, struct icaltimetype t);

int64_t recur_clock(void);
RecurResult recur_each(icalcomponent *c, const Recur *r);

#endif
