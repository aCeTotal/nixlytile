/* Google and Microsoft REST internals. */
#ifndef REST_H
#define REST_H

#include <json-c/json.h>

#include "calsync.h"

#define STAMP_MAX      32
#define TASK_DONE_JSON "{\"status\":\"completed\"}"
#define GRAPH_ME       "https://graph.microsoft.com/v1.0/me"

/* Unset page_param: next is absolute. */
typedef struct {
	const char *name;
	const char *items;
	const char *next;
	const char *page_param;
	const OAuthSpec *oauth;
} RestApi;

typedef struct {
	const RestApi *api;
	Account *a;
	SyncRun *run;
	const char *url, *etag, *prefer;
	json_object *items;
	char *next;
	char tag[256];
	HttpRes res;
} Pager;

typedef struct {
	int64_t from, to;
} Span;

typedef struct {
	CalMeta meta;
	const char *acct, *list;
} TaskList;

char *strfmt(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
json_object *rest_field(json_object *o, const char *key);
const char *rest_str(json_object *o, const char *key);
size_t rest_count(json_object *arr);
CalEvent *rest_push(EventList *out, const CalMeta *m, const char *title);
char *rest_span_url(const char *fmt, const char *id, Span s);
char *rest_task_url(const char *lists, const char *ref, SyncRun *run);
int rest_pages(Pager *p);
int rest_write(Pager *p, HttpReq *q);
int rest_fail(const RestApi *api, SyncRun *run, const HttpRes *r);
int rest_scope_fail(const RestApi *api, SyncRun *run, const HttpRes *r);

extern const RestApi google_api;
int64_t google_day(const char *s, int plus);

int google_tasks_sync(Account *a, SyncRun *run);

extern const RestApi graph_api;
int64_t graph_day(const char *s, int plus);

int graph_todo_sync(Account *a, SyncRun *run);

#endif
