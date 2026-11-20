/* CalDAV calendar sync. */
#include <libxml/xpath.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "dav.h"

#define STAMP_LEN   (sizeof "20260101T000000Z")
#define QUERY_MAX   1024
#define REF_LEN     (sizeof ((CalEvent *)0)->ref)
#define PIPE_ESC    "%7C"
#define COMP_EVENT  1u
#define COMP_TODO   2u
#define PROP        "d:propstat/d:prop/"
#define COMP_SET    PROP "c:supported-calendar-component-set/c:comp"
#define RESPONSES   "/d:multistatus/d:response"
#define QUERY_HEAD  "<c:calendar-query xmlns:d=\"" NS_DAV "\" xmlns:c=\"" \
	NS_CALDAV "\"><d:prop><c:calendar-data/></d:prop><c:filter>" \
	"<c:comp-filter name=\"VCALENDAR\">"
#define QUERY_TAIL  "</c:comp-filter></c:filter></c:calendar-query>"
#define EVENT_QUERY QUERY_HEAD "<c:comp-filter name=\"VEVENT\">" \
	"<c:time-range start=\"%s\" end=\"%s\"/></c:comp-filter>" QUERY_TAIL

typedef struct {
	char url[sizeof ((Account *)0)->todo_url];
	char name[sizeof ((CalEvent *)0)->cal];
	char tag[sizeof ((Source *)0)->tag];
	uint32_t color;
	unsigned comps;
} Calendar;

typedef struct {
	Dav list, fetch;
	EventList fresh;
	CalMeta meta;
	char event_query[QUERY_MAX];
	int failed;
} Sync;

static const char todo_query[] = QUERY_HEAD "<c:comp-filter name=\"VTODO\"/>"
	QUERY_TAIL;

static unsigned
components(const Dav *d, xmlNodePtr node)
{
	if (!dav_count(d, node, COMP_SET))
		return COMP_EVENT | COMP_TODO;
	return (dav_count(d, node, COMP_SET "[@name='VEVENT']") ? COMP_EVENT : 0)
		| (dav_count(d, node, COMP_SET "[@name='VTODO']") ? COMP_TODO : 0);
}

static void
last_segment(char *dst, size_t len, const char *url)
{
	size_t end = strlen(url), start;

	while (end && url[end - 1] == '/')
		end--;
	for (start = end; start && url[start - 1] != '/'; start--)
		;
	snprintf(dst, len, "%.*s", (int)(end - start), url + start);
}

/* Copy, then release libxml text. */
static void
take(char *dst, size_t len, xmlChar *s)
{
	snprintf(dst, len, "%s", s ? (const char *)s : "");
	xmlFree(s);
}

/* Zero comps: not a calendar. */
static Calendar
read_calendar(const Dav *d, xmlNodePtr node)
{
	Calendar c = { 0 };
	char *url = dav_count(d, node, PROP "d:resourcetype/c:calendar")
			? dav_href(d, node, "d:href") : NULL;
	char text[sizeof c.tag];

	if (!url || strlen(url) >= sizeof c.url) {
		free(url);
		return c;
	}
	snprintf(c.url, sizeof c.url, "%s", url);
	free(url);
	take(text, sizeof text, dav_text(d, node, PROP "d:displayname"));
	if (!text[0])
		last_segment(text, sizeof text, c.url);
	event_text(c.name, sizeof c.name, text);
	take(text, sizeof text, dav_text(d, node, PROP "cs:getctag"));
	if (!text[0])
		take(text, sizeof text, dav_text(d, node, PROP "d:sync-token"));
	event_text(c.tag, sizeof c.tag, text);
	take(text, sizeof text, dav_text(d, node, PROP "ic:calendar-color"));
	if (parse_color(text, &c.color))
		c.color = palette_color(c.name);
	c.comps = components(d, node);
	return c;
}

static void
each_response(Sync *s, const Dav *d, void (*fn)(Sync *, xmlNodePtr))
{
	xmlXPathObjectPtr o = xmlXPathEval(BAD_CAST RESPONSES, d->xp);
	xmlNodeSetPtr set = o ? o->nodesetval : NULL;

	for (int i = 0; i < xmlXPathNodeSetGetLength(set); i++)
		fn(s, xmlXPathNodeSetItem(set, i));
	xmlXPathFreeObject(o);
}

/* Empty when it would overflow. */
static void
task_ref(char *dst, const char *acct, const char *url)
{
	size_t n, k;

	memset(dst, 0, REF_LEN);
	if (!url)
		return;
	n = (size_t)snprintf(dst, REF_LEN, "%s|", acct);
	for (; *url; url++, n += k) {
		k = *url == '|' ? sizeof PIPE_ESC - 1 : 1;
		if (n + k >= REF_LEN) {
			memset(dst, 0, REF_LEN);
			return;
		}
		memcpy(dst + n, *url == '|' ? PIPE_ESC : url, k);
	}
}

static void
link_tasks(Sync *s, xmlNodePtr node, size_t from)
{
	CalEvent *v = s->fresh.v;
	size_t i = from;
	char ref[REF_LEN];
	char *url;

	while (i < s->fresh.n && !(v[i].flags & EV_TASK))
		i++;
	if (i == s->fresh.n)
		return;
	url = dav_href(&s->fetch, node, "d:href");
	task_ref(ref, s->fetch.a->id, url);
	free(url);
	for (; i < s->fresh.n; i++)
		if (v[i].flags & EV_TASK)
			memcpy(v[i].ref, ref, sizeof ref);
}

static void
expand_resource(Sync *s, xmlNodePtr node)
{
	xmlChar *ics = dav_text(&s->fetch, node, PROP "c:calendar-data");
	size_t from = s->fresh.n;

	if (!ics)
		return;
	ical_expand((const char *)ics, &s->meta, &s->fetch.run->w, &s->fresh);
	xmlFree(ics);
	link_tasks(s, node, from);
}

static int
fetch(Sync *s, const char *url, const char *query)
{
	HttpReq q = { .method = "REPORT", .url = url, .depth = "1", .body = query };

	if (dav_query(&s->fetch, q) < 0)
		return -1;
	each_response(s, &s->fetch, expand_resource);
	return 0;
}

static int
refresh(Sync *s, Source *src, const Calendar *c)
{
	s->fresh = (EventList){ 0 };
	s->meta = (CalMeta){ c->name, c->color };
	if (((c->comps & COMP_EVENT) && fetch(s, c->url, s->event_query) < 0)
			|| ((c->comps & COMP_TODO) && fetch(s, c->url, todo_query) < 0)) {
		events_free(&s->fresh);
		return -1;
	}
	source_set_tag(src, c->tag);
	source_replace(src, &s->fresh, &s->fetch.run->w);
	return 0;
}

static int
sync_calendar(Sync *s, const Calendar *c)
{
	char id[sizeof "cal:" + sizeof c->url];
	Source src;
	int rc = 0;

	snprintf(id, sizeof id, "cal:%s", c->url);
	source_open(&src, s->fetch.a, id);
	if (source_full_due(&src, &s->fetch.run->w) || strcmp(src.tag, c->tag))
		rc = refresh(s, &src, c);
	source_close(&src, s->fetch.run);
	return rc;
}

static void
visit(Sync *s, xmlNodePtr node)
{
	Calendar c = read_calendar(&s->list, node);
	Account *a = s->list.a;

	if (!c.comps)
		return;
	if (c.comps & COMP_EVENT)
		a->ncals++;
	if ((c.comps & COMP_TODO) && !a->todo_url[0])
		snprintf(a->todo_url, sizeof a->todo_url, "%s", c.url);
	if (sync_calendar(s, &c) < 0)
		s->failed = 1;
}

static void
stamp(char *dst, int64_t t)
{
	time_t v = (time_t)t;
	struct tm tm;

	strftime(dst, STAMP_LEN, "%Y%m%dT%H%M%SZ", gmtime_r(&v, &tm));
}

int
caldav_sync(Account *a, SyncRun *run)
{
	Sync s = { .list = { .a = a, .run = run }, .fetch = { .a = a, .run = run } };
	char from[STAMP_LEN], to[STAMP_LEN];

	if (dav_list_home(&s.list) < 0) {
		dav_reset(&s.list);
		return -1;
	}
	stamp(from, run->w.full_from);
	stamp(to, run->w.full_to);
	snprintf(s.event_query, sizeof s.event_query, EVENT_QUERY, from, to);
	a->ncals = 0;
	a->todo_url[0] = '\0';
	each_response(&s, &s.list, visit);
	dav_reset(&s.list);
	dav_reset(&s.fetch);
	return s.failed ? -1 : 0;
}
