/* Google Calendar. */
#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rest.h"

#define CAL_API   "https://www.googleapis.com/calendar/v3"
#define LABEL_URL CAL_API "/users/me/calendarList?minAccessRole=owner" \
		"&fields=items(id,primary)"
#define CALS_URL  CAL_API "/users/me/calendarList?maxResults=250" \
		"&fields=nextPageToken,items(id,summary,summaryOverride," \
		"backgroundColor,selected,deleted,hidden)"
#define EVENTS_URL CAL_API "/calendars/%s/events?timeMin=%s&timeMax=%s" \
		"&singleEvents=true&showDeleted=false&maxResults=2500" \
		"&fields=etag,nextPageToken,items(summary,location,start,end," \
		"status,eventType,attendees(self,responseStatus))"
#define DATE_PARTS 3

/* Refresh keeps the granted scopes. */
const OAuthSpec google_oauth = {
	.name = "Google",
	.auth_url = "https://accounts.google.com/o/oauth2/v2/auth",
	.token_url = "https://oauth2.googleapis.com/token",
	.scope = "https://www.googleapis.com/auth/calendar.readonly "
			"https://www.googleapis.com/auth/tasks",
	.redirect_host = "127.0.0.1",
	.auth_extra = "access_type=offline&prompt=consent",
};

const RestApi google_api = {
	.name = "Google",
	.items = "items",
	.next = "nextPageToken",
	.page_param = "&pageToken=",
	.oauth = &google_oauth,
};

static int
flag(json_object *o, const char *key)
{
	return json_object_get_boolean(rest_field(o, key));
}

/* Local midnight of date prefix. */
int64_t
google_day(const char *s, int plus)
{
	int y, m, d;

	if (sscanf(s, "%4d-%2d-%2d", &y, &m, &d) != DATE_PARTS)
		return INT64_MIN;
	return local_midnight(y, m, d + plus);
}

/* Cancelled, declined or working location. */
static int
skipped(json_object *ev)
{
	json_object *att = rest_field(ev, "attendees");
	size_t i, n = rest_count(att);

	if (!strcmp(rest_str(ev, "status"), "cancelled")
			|| !strcmp(rest_str(ev, "eventType"), "workingLocation"))
		return 1;
	for (i = 0; i < n; i++) {
		json_object *who = json_object_array_get_idx(att, i);

		if (flag(who, "self"))
			return !strcmp(rest_str(who, "responseStatus"), "declined");
	}
	return 0;
}

static void
parse_events(json_object *items, const CalMeta *m, EventList *out)
{
	size_t i, n = rest_count(items);

	for (i = 0; i < n; i++) {
		json_object *ev = json_object_array_get_idx(items, i);
		json_object *start = rest_field(ev, "start");
		json_object *end = rest_field(ev, "end");
		const char *day = rest_str(start, "date");
		int64_t from = *day ? google_day(day, 0)
				: parse_rfc3339(rest_str(start, "dateTime"));
		int64_t to = *day ? google_day(rest_str(end, "date"), 0)
				: parse_rfc3339(rest_str(end, "dateTime"));
		CalEvent *e;

		if (*day && to <= from)
			to = google_day(day, 1);
		if (from == INT64_MIN || to == INT64_MIN || skipped(ev))
			continue;
		e = rest_push(out, m, rest_str(ev, "summary"));
		e->start = from;
		e->end = to;
		e->flags = *day ? EV_ALLDAY : 0;
		event_text(e->where, sizeof e->where, rest_str(ev, "location"));
	}
}

static CalMeta
calendar_meta(json_object *cal)
{
	const char *name = rest_str(cal, "summaryOverride");
	uint32_t rgb;

	if (!*name)
		name = rest_str(cal, "summary");
	if (parse_color(rest_str(cal, "backgroundColor"), &rgb) < 0)
		rgb = palette_color(name);
	return (CalMeta){ name, rgb };
}

/* Near window, full when due. */
static int
calendar_sync(Account *a, SyncRun *run, json_object *cal)
{
	const char *id = rest_str(cal, "id");
	CalMeta m = calendar_meta(cal);
	char *key = strfmt("cal:%s", id), *url;
	Pager p = { .api = &google_api, .a = a, .run = run };
	EventList fresh = { 0 };
	Source s;
	int full, got;

	source_open(&s, a, key);
	free(key);
	full = source_full_due(&s, &run->w);
	p.etag = !full && s.tag[0] ? s.tag : NULL;
	p.url = url = rest_span_url(EVENTS_URL, id, full
			? (Span){ run->w.full_from, run->w.full_to }
			: (Span){ run->w.near_from, run->w.near_to });
	got = !rest_pages(&p);
	if (got) {
		parse_events(p.items, &m, &fresh);
		source_set_tag(&s, p.tag);
		(full ? source_replace : source_splice)(&s, &fresh, &run->w);
	}
	source_close(&s, run);
	json_object_put(p.items);
	free(url);
	if (got || p.res.status == HTTP_NOT_MODIFIED
			|| p.res.status == HTTP_FORBIDDEN || p.res.status == HTTP_NOT_FOUND)
		return 0;
	return rest_fail(&google_api, run, &p.res);
}

static int
calendars_sync(Account *a, SyncRun *run)
{
	Pager p = { .api = &google_api, .a = a, .run = run, .url = CALS_URL };
	int rc = rest_pages(&p) < 0 ? rest_fail(&google_api, run, &p.res) : 0;
	size_t i, n = rc ? 0 : rest_count(p.items);
	int used = 0;

	for (i = 0; i < n && !rc; i++) {
		json_object *cal = json_object_array_get_idx(p.items, i);

		if (!*rest_str(cal, "id") || !flag(cal, "selected")
				|| flag(cal, "deleted") || flag(cal, "hidden"))
			continue;
		used++;
		rc = calendar_sync(a, run, cal);
	}
	if (!rc)
		a->ncals = used;
	json_object_put(p.items);
	return rc;
}

/* Primary calendar, else the first. */
static json_object *
primary(json_object *cals)
{
	size_t i, n = rest_count(cals);

	for (i = 0; i < n; i++)
		if (flag(json_object_array_get_idx(cals, i), "primary"))
			return json_object_array_get_idx(cals, i);
	return json_object_array_get_idx(cals, 0);
}

int
google_label(Account *a, SyncRun *run)
{
	Pager p = { .api = &google_api, .a = a, .run = run, .url = LABEL_URL };
	int rc;

	if (oauth_token(&google_oauth, a, run) < 0)
		return -1;
	rc = rest_pages(&p) < 0 ? rest_fail(&google_api, run, &p.res) : 0;
	snprintf(a->label, sizeof a->label, "%s", rest_str(primary(p.items), "id"));
	json_object_put(p.items);
	return rc;
}

int
google_sync(Account *a, SyncRun *run)
{
	if (oauth_token(&google_oauth, a, run) < 0 || calendars_sync(a, run) < 0)
		return -1;
	return google_tasks_sync(a, run);
}
