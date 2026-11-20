/* Microsoft To Do lists. */
#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rest.h"

#define LISTS_URL  GRAPH_ME "/todo/lists?$top=100"
#define TASKS_URL  GRAPH_ME "/todo/lists/%s/tasks?$filter=%s" \
		"&$select=id,title,status,dueDateTime&$top=100"
#define ADD_URL    GRAPH_ME "/todo/lists/%s/tasks"
#define TASK_LISTS GRAPH_ME "/todo/lists"
#define OPEN_ONLY  "status%20ne%20%27completed%27"
#define DUE_FMT    "%04d-%02d-%02dT12:00:00"

/* Overlong ids leave tasks uncompletable. */
static void
parse_tasks(json_object *items, const TaskList *l, EventList *out)
{
	size_t i, n = rest_count(items);

	for (i = 0; i < n; i++) {
		json_object *t = json_object_array_get_idx(items, i);
		const char *due = rest_str(rest_field(t, "dueDateTime"), "dateTime");
		int64_t from = *due ? graph_day(due, 0) : 0;
		CalEvent *e;

		if (!strcmp(rest_str(t, "status"), "completed") || from == INT64_MIN)
			continue;
		e = rest_push(out, &l->meta, rest_str(t, "title"));
		e->flags = *due ? EV_TASK | EV_ALLDAY : EV_TASK;
		e->start = from;
		e->end = *due ? graph_day(due, 1) : 0;
		if (snprintf(e->ref, sizeof e->ref, "%s|%s|%s", l->acct, l->list,
				rest_str(t, "id")) >= (int)sizeof e->ref)
			memset(e->ref, 0, sizeof e->ref);
	}
}

/* Rewritten only when tasks change. */
static int
todolist_sync(Account *a, SyncRun *run, json_object *list)
{
	const char *name = rest_str(list, "displayName");
	TaskList l = { { name, palette_color(name) }, a->id, rest_str(list, "id") };
	char *esc = http_esc(l.list), *url = strfmt(TASKS_URL, esc, OPEN_ONLY);
	char *key = strfmt("todo:%s", l.list);
	Pager p = { .api = &graph_api, .a = a, .run = run, .url = url };
	EventList fresh = { 0 };
	int got = !rest_pages(&p);
	Source s;

	if (got) {
		source_open(&s, a, key);
		parse_tasks(p.items, &l, &fresh);
		source_take(&s, &fresh);
		source_close(&s, run);
	}
	json_object_put(p.items);
	free(esc);
	free(url);
	free(key);
	return got ? 0 : rest_fail(&graph_api, run, &p.res);
}

/* Without To Do access, skip. */
int
graph_todo_sync(Account *a, SyncRun *run)
{
	Pager p = { .api = &graph_api, .a = a, .run = run, .url = LISTS_URL };
	int got = !rest_pages(&p);
	size_t i, n = got ? rest_count(p.items) : 0;
	int rc = got || p.res.status == HTTP_FORBIDDEN
			|| p.res.status == HTTP_NOT_FOUND ? 0
			: rest_fail(&graph_api, run, &p.res);

	for (i = 0; i < n && !rc; i++)
		rc = todolist_sync(a, run, json_object_array_get_idx(p.items, i));
	json_object_put(p.items);
	return rc;
}

/* defaultList, else the first. */
static json_object *
default_list(json_object *lists)
{
	size_t i, n = rest_count(lists);

	for (i = 0; i < n; i++) {
		json_object *l = json_object_array_get_idx(lists, i);

		if (!strcmp(rest_str(l, "wellknownListName"), "defaultList"))
			return l;
	}
	return json_object_array_get_idx(lists, 0);
}

static json_object *
task_json(const TodoNew *t)
{
	json_object *task = json_object_new_object(), *due;
	char stamp[STAMP_MAX];

	json_object_object_add(task, "title", json_object_new_string(t->title));
	if (!t->y)
		return task;
	due = json_object_new_object();
	snprintf(stamp, sizeof stamp, DUE_FMT, t->y, t->m, t->d);
	json_object_object_add(due, "dateTime", json_object_new_string(stamp));
	json_object_object_add(due, "timeZone", json_object_new_string("UTC"));
	json_object_object_add(task, "dueDateTime", due);
	return task;
}

int
graph_todo_add(Account *a, const TodoNew *t, SyncRun *run)
{
	Pager p = { .api = &graph_api, .a = a, .run = run, .url = LISTS_URL };
	HttpReq q = { .method = "POST", .bearer = a->access };
	json_object *task;
	char *esc, *url;
	int rc;

	if (oauth_token(&graph_oauth, a, run) < 0)
		return -1;
	if (rest_pages(&p) < 0) {
		json_object_put(p.items);
		return rest_scope_fail(&graph_api, run, &p.res);
	}
	esc = http_esc(rest_str(default_list(p.items), "id"));
	json_object_put(p.items);
	task = task_json(t);
	q.url = url = strfmt(ADD_URL, esc);
	q.body = json_object_to_json_string_ext(task, JSON_C_TO_STRING_PLAIN);
	rc = rest_write(&p, &q);
	json_object_put(task);
	free(url);
	free(esc);
	return rc;
}

int
graph_todo_done(Account *a, const char *ref, SyncRun *run)
{
	Pager p = { .api = &graph_api, .a = a, .run = run };
	char *url = rest_task_url(TASK_LISTS, ref, run);
	HttpReq q = { .method = "PATCH", .url = url, .bearer = a->access,
			.body = TASK_DONE_JSON };
	int rc = -1;

	if (url && !oauth_token(&graph_oauth, a, run))
		rc = rest_write(&p, &q);
	free(url);
	return rc;
}
