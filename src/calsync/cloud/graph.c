/* Microsoft Graph calendars. */
#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "rest.h"

#define LABEL_URL GRAPH_ME "?$select=mail,userPrincipalName"
#define CALS_URL  GRAPH_ME "/calendars?$select=id,name,hexColor,color&$top=100"
#define VIEW_URL  GRAPH_ME "/calendars/%s/calendarView?startDateTime=%s" \
		"&endDateTime=%s&$select=subject,start,end,isAllDay,location," \
		"isCancelled,responseStatus&$top=250"
#define IN_UTC    "outlook.timezone=\"UTC\""
#define SCOPE     "offline_access User.Read Calendars.Read Tasks.ReadWrite"
#define LENGTH(a) (sizeof(a) / sizeof((a)[0]))
#define DAY_S     86400
#define YEAR_BASE 1900

static const struct {
	const char *name;
	uint32_t rgb;
} tones[] = {
	{ "lightBlue", 0x4a9ff5 }, { "lightGreen", 0x5cc28a },
	{ "lightOrange", 0xf5a35a }, { "lightGray", 0x9aa0a6 },
	{ "lightYellow", 0xf2cf5b }, { "lightTeal", 0x3dc5c0 },
	{ "lightPink", 0xf28cb1 }, { "lightBrown", 0xc49a6c },
	{ "lightRed", 0xef6f6c },
};

const OAuthSpec graph_oauth = {
	.name = "Microsoft",
	.auth_url = "https://login.microsoftonline.com/common/oauth2/v2.0/authorize",
	.token_url = "https://login.microsoftonline.com/common/oauth2/v2.0/token",
	.scope = SCOPE,
	.redirect_host = "localhost",
	.auth_extra = "prompt=select_account",
	.refresh_scope = SCOPE,
};

const RestApi graph_api = {
	.name = "Microsoft",
	.items = "value",
	.next = "@odata.nextLink",
	.oauth = &graph_oauth,
};

/* UTC midnight means floating date. */
int64_t
graph_day(const char *s, int plus)
{
	int64_t t = parse_rfc3339(s);
	time_t tt = (time_t)t;
	struct tm tm;

	if (t == INT64_MIN)
		return INT64_MIN;
	if (t % DAY_S)
		localtime_r(&tt, &tm);
	else
		gmtime_r(&tt, &tm);
	return local_midnight(tm.tm_year + YEAR_BASE, tm.tm_mon + 1,
			tm.tm_mday + plus);
}

static void
parse_events(json_object *items, const CalMeta *m, EventList *out)
{
	size_t i, n = rest_count(items);

	for (i = 0; i < n; i++) {
		json_object *ev = json_object_array_get_idx(items, i);
		const char *start = rest_str(rest_field(ev, "start"), "dateTime");
		const char *end = rest_str(rest_field(ev, "end"), "dateTime");
		int day = json_object_get_boolean(rest_field(ev, "isAllDay"));
		int64_t from = day ? graph_day(start, 0) : parse_rfc3339(start);
		int64_t to = day ? graph_day(end, 0) : parse_rfc3339(end);
		CalEvent *e;

		if (day && to <= from)
			to = graph_day(start, 1);
		if (from == INT64_MIN || to == INT64_MIN
				|| json_object_get_boolean(rest_field(ev, "isCancelled"))
				|| !strcmp(rest_str(rest_field(ev, "responseStatus"), "response"),
						"declined"))
			continue;
		e = rest_push(out, m, rest_str(ev, "subject"));
		e->start = from;
		e->end = to;
		e->flags = day ? EV_ALLDAY : 0;
		event_text(e->where, sizeof e->where,
				rest_str(rest_field(ev, "location"), "displayName"));
	}
}

static uint32_t
calendar_color(json_object *cal, const char *name)
{
	const char *tone = rest_str(cal, "color");
	uint32_t rgb;
	size_t i;

	if (!parse_color(rest_str(cal, "hexColor"), &rgb))
		return rgb;
	for (i = 0; i < LENGTH(tones); i++)
		if (!strcmp(tone, tones[i].name))
			return tones[i].rgb;
	return palette_color(name);
}

/* Near window, full when due. */
static int
calendar_sync(Account *a, SyncRun *run, json_object *cal)
{
	const char *id = rest_str(cal, "id"), *name = rest_str(cal, "name");
	CalMeta m = { name, calendar_color(cal, name) };
	char *key = strfmt("cal:%s", id), *url;
	Pager p = { .api = &graph_api, .a = a, .run = run, .prefer = IN_UTC };
	EventList fresh = { 0 };
	Source s;
	int full, got;

	source_open(&s, a, key);
	free(key);
	full = source_full_due(&s, &run->w);
	p.url = url = rest_span_url(VIEW_URL, id, full
			? (Span){ run->w.full_from, run->w.full_to }
			: (Span){ run->w.near_from, run->w.near_to });
	got = !rest_pages(&p);
	if (got) {
		parse_events(p.items, &m, &fresh);
		(full ? source_replace : source_splice)(&s, &fresh, &run->w);
	}
	source_close(&s, run);
	json_object_put(p.items);
	free(url);
	if (got || p.res.status == HTTP_FORBIDDEN || p.res.status == HTTP_NOT_FOUND)
		return 0;
	return rest_fail(&graph_api, run, &p.res);
}

static int
calendars_sync(Account *a, SyncRun *run)
{
	Pager p = { .api = &graph_api, .a = a, .run = run, .url = CALS_URL };
	int rc = rest_pages(&p) < 0 ? rest_fail(&graph_api, run, &p.res) : 0;
	size_t i, n = rc ? 0 : rest_count(p.items);

	for (i = 0; i < n && !rc; i++)
		rc = calendar_sync(a, run, json_object_array_get_idx(p.items, i));
	if (!rc)
		a->ncals = (int)n;
	json_object_put(p.items);
	return rc;
}

int
graph_label(Account *a, SyncRun *run)
{
	HttpReq q = { .url = LABEL_URL, .bearer = a->access };
	const char *mail;
	json_object *me;
	HttpRes r;
	int rc;

	if (oauth_token(&graph_oauth, a, run) < 0)
		return -1;
	r = http_send(&q);
	me = r.status == HTTP_OK ? json_tokener_parse(r.body) : NULL;
	rc = me ? 0 : rest_fail(&graph_api, run, &r);
	mail = rest_str(me, "mail");
	snprintf(a->label, sizeof a->label, "%s",
			*mail ? mail : rest_str(me, "userPrincipalName"));
	json_object_put(me);
	http_res_free(&r);
	return rc;
}

int
graph_sync(Account *a, SyncRun *run)
{
	if (oauth_token(&graph_oauth, a, run) < 0 || calendars_sync(a, run) < 0)
		return -1;
	return graph_todo_sync(a, run);
}
