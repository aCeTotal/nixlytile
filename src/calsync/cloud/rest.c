/* JSON REST plumbing for providers. */
#include <json-c/json.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "rest.h"

#define JSON_TYPE "application/json"
#define ESC_BAR   "%7C"
#define NO_TITLE  "(No title)"

char *
strfmt(const char *fmt, ...)
{
	va_list ap;
	char *s;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);
	s = xrealloc(NULL, (size_t)n + 1);
	va_start(ap, fmt);
	vsnprintf(s, (size_t)n + 1, fmt, ap);
	va_end(ap);
	return s;
}

json_object *
rest_field(json_object *o, const char *key)
{
	json_object *v;

	return json_object_object_get_ex(o, key, &v) ? v : NULL;
}

/* String member, else "". */
const char *
rest_str(json_object *o, const char *key)
{
	json_object *v = rest_field(o, key);

	return json_object_is_type(v, json_type_string)
			? json_object_get_string(v) : "";
}

size_t
rest_count(json_object *arr)
{
	return json_object_is_type(arr, json_type_array)
			? json_object_array_length(arr) : 0;
}

CalEvent *
rest_push(EventList *out, const CalMeta *m, const char *title)
{
	CalEvent *e = events_push(out);

	event_text(e->title, sizeof e->title, title);
	if (!e->title[0])
		strcpy(e->title, NO_TITLE);
	event_text(e->cal, sizeof e->cal, m->name);
	e->color = m->color;
	return e;
}

static void
utc(char *buf, int64_t t)
{
	time_t tt = (time_t)t;
	struct tm tm;

	strftime(buf, STAMP_MAX, "%Y-%m-%dT%H:%M:%SZ", gmtime_r(&tt, &tm));
}

/* fmt takes id, start, end. */
char *
rest_span_url(const char *fmt, const char *id, Span s)
{
	char from[STAMP_MAX], to[STAMP_MAX];
	char *esc = http_esc(id), *url;

	utc(from, s.from);
	utc(to, s.to);
	url = strfmt(fmt, esc, from, to);
	free(esc);
	return url;
}

/* Split "<list>|<task>" once escaped. */
char *
rest_task_url(const char *lists, const char *ref, SyncRun *run)
{
	char *esc, *bar, *url;

	if (!strchr(ref, '|')) {
		run_fail(run, "Unknown to-do");
		return NULL;
	}
	esc = http_esc(ref);
	bar = strstr(esc, ESC_BAR);
	url = strfmt("%s/%.*s/tasks/%s", lists, (int)(bar - esc), esc,
			bar + strlen(ESC_BAR));
	free(esc);
	return url;
}

static char *
next_url(const Pager *p, const char *next)
{
	char *esc, *url;

	if (!*next)
		return NULL;
	if (!p->api->page_param)
		return strfmt("%s", next);
	esc = http_esc(next);
	url = strfmt("%s%s%s", p->url, p->api->page_param, esc);
	free(esc);
	return url;
}

/* Append items, queue next page. */
static int
page_take(Pager *p, const char *body)
{
	json_object *root = json_tokener_parse(body);
	json_object *items = rest_field(root, p->api->items);
	size_t i, n = rest_count(items);

	if (!root)
		return -1;
	if (!p->tag[0])
		snprintf(p->tag, sizeof p->tag, "%s", rest_str(root, "etag"));
	for (i = 0; i < n; i++)
		json_object_array_add(p->items,
				json_object_get(json_object_array_get_idx(items, i)));
	free(p->next);
	p->next = next_url(p, rest_str(root, p->api->next));
	json_object_put(root);
	return 0;
}

/* Refused tokens get one refresh. */
static HttpRes
send_authed(Pager *p, const HttpReq *q)
{
	HttpRes r = http_send(q);

	if (r.status != HTTP_UNAUTHORIZED)
		return r;
	p->a->access[0] = '\0';
	p->a->access_exp = 0;
	if (oauth_token(p->api->oauth, p->a, p->run) < 0)
		return r;
	http_res_free(&r);
	return http_send(q);
}

/* Every page into p->items. */
int
rest_pages(Pager *p)
{
	HttpReq q = { .url = p->url, .bearer = p->a->access,
			.if_none_match = p->etag, .prefer = p->prefer };
	int failed;

	p->items = json_object_new_array();
	do {
		p->res = send_authed(p, &q);
		failed = p->res.status != HTTP_OK || page_take(p, p->res.body) < 0;
		http_res_free(&p->res);
		q.url = p->next;
		q.if_none_match = NULL;
	} while (!failed && q.url);
	free(p->next);
	return failed ? -1 : 0;
}

int
rest_write(Pager *p, HttpReq *q)
{
	HttpRes r;
	int rc;

	q->ctype = JSON_TYPE;
	r = send_authed(p, q);
	rc = r.status == HTTP_OK || r.status == HTTP_CREATED ? 0
			: rest_scope_fail(p->api, p->run, &r);
	http_res_free(&r);
	return rc;
}

/* A failed 200 was unreadable. */
int
rest_fail(const RestApi *api, SyncRun *run, const HttpRes *r)
{
	switch (r->status) {
	case 0:
		run_fail(run, "Offline: %s", r->error);
		break;
	case HTTP_OK:
		run_fail(run, "%s sent an unreadable reply", api->name);
		break;
	case HTTP_UNAUTHORIZED:
		run_fail(run, "%s rejected the sign-in — add the account again",
				api->name);
		break;
	default:
		run_fail(run, "%s: HTTP %ld", api->name, r->status);
	}
	return -1;
}

/* Forbidden means missing task scope. */
int
rest_scope_fail(const RestApi *api, SyncRun *run, const HttpRes *r)
{
	if (r->status != HTTP_FORBIDDEN)
		return rest_fail(api, run, r);
	run_fail(run, "%s sign-in lacks to-do access — add the account again",
			api->name);
	return -1;
}
