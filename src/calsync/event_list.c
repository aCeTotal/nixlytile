/* Event lists and line format. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "calsync.h"

#define EVENTS_MIN      64
#define EVENT_FIELDS    8
#define LINE_SLACK      64

CalEvent *
events_push(EventList *l)
{
	CalEvent *e;

	if (l->n == l->cap) {
		l->cap = l->cap ? l->cap * 2 : EVENTS_MIN;
		l->v = xrealloc(l->v, l->cap * sizeof *l->v);
	}
	e = &l->v[l->n++];
	memset(e, 0, sizeof *e);
	return e;
}

void
events_free(EventList *l)
{
	free(l->v);
	memset(l, 0, sizeof *l);
}

/* Total order keeps memcmp stable. */
static int
event_cmp(const void *pa, const void *pb)
{
	const CalEvent *a = pa, *b = pb;
	int c = (a->start > b->start) - (a->start < b->start);

	if (!c)
		c = (a->end > b->end) - (a->end < b->end);
	if (!c)
		c = strcmp(a->title, b->title);
	if (!c)
		c = strcmp(a->ref, b->ref);
	return c ? c : memcmp(a, b, sizeof *a);
}

void
events_sort(EventList *l)
{
	if (l->n > 1)
		qsort(l->v, l->n, sizeof *l->v, event_cmp);
}

int
events_equal(const EventList *a, const EventList *b)
{
	return a->n == b->n
			&& (!a->n || !memcmp(a->v, b->v, a->n * sizeof *a->v));
}

char *
events_format(const EventList *l, const char *head)
{
	size_t cap = l->n * (sizeof(CalEvent) + LINE_SLACK) + 1;
	size_t n = 0;
	char *buf;

	if (head)
		cap += strlen(head) + 1;
	buf = xrealloc(NULL, cap);
	buf[0] = '\0';
	if (head)
		n = (size_t)snprintf(buf, cap, "%s\n", head);
	for (size_t i = 0; i < l->n; i++) {
		const CalEvent *e = &l->v[i];

		n += (size_t)snprintf(buf + n, cap - n,
				"%lld\t%lld\t%u\t%06x\t%s\t%s\t%s\t%s\n",
				(long long)e->start, (long long)e->end, e->flags,
				(unsigned)e->color, e->title, e->where, e->cal, e->ref);
	}
	return buf;
}

int
tab_split(char *line, char **f, int max)
{
	int n = 0;

	while (line && n < max)
		f[n++] = strsep(&line, "\t");
	return line ? -1 : n;
}

static int
field_copy(char *dst, size_t size, const char *s)
{
	size_t n = strlen(s);

	if (n >= size)
		return -1;
	memcpy(dst, s, n + 1);
	return 0;
}

/* Whole decimal string, else INT64_MIN. */
static int64_t
number(const char *s)
{
	char *end;
	long long v;

	errno = 0;
	v = strtoll(s, &end, 10);
	if (end == s || *end || errno)
		return INT64_MIN;
	return v;
}

static void
parse_line(EventList *l, char *line)
{
	char *f[EVENT_FIELDS];
	int64_t start, end, flags;
	CalEvent *e;

	line[strcspn(line, "\n")] = '\0';
	if (line[0] == '#' || tab_split(line, f, EVENT_FIELDS) != EVENT_FIELDS)
		return;
	start = number(f[0]);
	end = number(f[1]);
	flags = number(f[2]);
	if (start == INT64_MIN || end == INT64_MIN || flags < 0
			|| flags > UINT32_MAX || strlen(f[3]) != RGB_DIGITS
			|| strspn(f[3], HEX_DIGITS) != RGB_DIGITS)
		return;
	e = events_push(l);
	e->start = start;
	e->end = end;
	e->flags = (unsigned)flags;
	e->color = (uint32_t)strtoul(f[3], NULL, 16);
	if (field_copy(e->title, sizeof e->title, f[4]) < 0
			|| field_copy(e->where, sizeof e->where, f[5]) < 0
			|| field_copy(e->cal, sizeof e->cal, f[6]) < 0
			|| field_copy(e->ref, sizeof e->ref, f[7]) < 0)
		l->n--;
}

void
events_load(EventList *l, const char *path)
{
	FILE *f = fopen(path, "re");
	char *line = NULL;
	size_t size = 0;

	if (!f)
		return;
	while (getline(&line, &size, f) > 0)
		parse_line(l, line);
	free(line);
	fclose(f);
}
