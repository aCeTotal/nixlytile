/* Google Tasks. */
#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rest.h"

#define TASKS_API  "https://tasks.googleapis.com/tasks/v1"
#define LISTS_URL  TASKS_API "/users/@me/lists?maxResults=100"
#define TASKS_URL  TASKS_API "/lists/%s/tasks?showCompleted=false" \
		"&showHidden=false&maxResults=100" \
		"&fields=etag,nextPageToken,items(id,title,due,status)"
#define ADD_URL    TASKS_API "/lists/@default/tasks"
#define TASK_LISTS TASKS_API "/lists"
#define DUE_FMT    "%04d-%02d-%02dT00:00:00.000Z"

static void
parse_tasks(json_object *items, const TaskList *l, EventList *out)
{
	size_t i, n = rest_count(items);

	for (i = 0; i < n; i++) {
		json_object *t = json_object_array_get_idx(items, i);
		const char *due = rest_str(t, "due");
		int64_t from = *due ? google_day(due, 0) : 0;
		CalEvent *e;

		if (!strcmp(rest_str(t, "status"), "completed") || from == INT64_MIN)
			continue;
		e = rest_push(out, &l->meta, rest_str(t, "title"));
		e->flags = *due ? EV_TASK | EV_ALLDAY : EV_TASK;
		e->start = from;
		e->end = *due ? google_day(due, 1) : 0;
		snprintf(e->ref, sizeof e->ref, "%s|%s|%s", l->acct, l->list,
				rest_str(t, "id"));
	}
}

static int
tasklist_sync(Account *a, SyncRun *run, json_object *list)
{
	const char *name = rest_str(list, "title");
	TaskList l = { { name, palette_color(name) }, a->id, rest_str(list, "id") };
	char *key = strfmt("tasks:%s", l.list), *esc = http_esc(l.list);
	char *url = strfmt(TASKS_URL, esc);
	Pager p = { .api = &google_api, .a = a, .run = run, .url = url };
	EventList fresh = { 0 };
	Source s;
	int got;

	source_open(&s, a, key);
	p.etag = !source_full_due(&s, &run->w) && s.tag[0] ? s.tag : NULL;
	got = !rest_pages(&p);
	if (got) {
		parse_tasks(p.items, &l, &fresh);
		source_set_tag(&s, p.tag);
		source_replace(&s, &fresh, &run->w);
	}
	source_close(&s, run);
	json_object_put(p.items);
	free(key);
	free(esc);
	free(url);
	return got || p.res.status == HTTP_NOT_MODIFIED ? 0
			: rest_fail(&google_api, run, &p.res);
}

/* Missing task scope: no tasks. */
int
google_tasks_sync(Account *a, SyncRun *run)
{
	Pager p = { .api = &google_api, .a = a, .run = run, .url = LISTS_URL };
	int got = !rest_pages(&p);
	size_t i, n = got ? rest_count(p.items) : 0;
	int rc = got || p.res.status == HTTP_FORBIDDEN ? 0
			: rest_fail(&google_api, run, &p.res);

	for (i = 0; i < n && !rc; i++)
		rc = tasklist_sync(a, run, json_object_array_get_idx(p.items, i));
	json_object_put(p.items);
	return rc;
}

static int
task_write(Account *a, SyncRun *run, HttpReq *q)
{
	Pager p = { .api = &google_api, .a = a, .run = run };

	if (oauth_token(&google_oauth, a, run) < 0)
		return -1;
	q->bearer = a->access;
	return rest_write(&p, q);
}

int
google_todo_add(Account *a, const TodoNew *t, SyncRun *run)
{
	json_object *task = json_object_new_object();
	HttpReq q = { .method = "POST", .url = ADD_URL };
	char due[STAMP_MAX];
	int rc;

	json_object_object_add(task, "title", json_object_new_string(t->title));
	if (t->y) {
		snprintf(due, sizeof due, DUE_FMT, t->y, t->m, t->d);
		json_object_object_add(task, "due", json_object_new_string(due));
	}
	q.body = json_object_to_json_string_ext(task, JSON_C_TO_STRING_PLAIN);
	rc = task_write(a, run, &q);
	json_object_put(task);
	return rc;
}

int
google_todo_done(Account *a, const char *ref, SyncRun *run)
{
	char *url = rest_task_url(TASK_LISTS, ref, run);
	HttpReq q = { .method = "PATCH", .url = url, .body = TASK_DONE_JSON };
	int rc = url ? task_write(a, run, &q) : -1;

	free(url);
	return rc;
}
