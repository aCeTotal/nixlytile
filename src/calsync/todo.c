/* Local and remote to-do edits. */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/random.h>
#include "calsync.h"

#define LOCAL_ID    "local"
#define LOCAL_CAL   "Local"
#define LOCAL_COLOR 0x8b93a6
#define NO_TITLE    "(No title)"
#define LENGTH(a)   (sizeof (a) / sizeof *(a))

typedef struct {
	const char *provider;
	int (*add)(Account *a, const TodoNew *t, SyncRun *run);
	int (*done)(Account *a, const char *ref, SyncRun *run);
} Backend;

static const Backend backends[] = {
	{ "google", google_todo_add, google_todo_done },
	{ "microsoft", graph_todo_add, graph_todo_done },
	{ "ics", NULL, NULL },
};

/* Every other provider speaks CalDAV. */
static const Backend caldav = { "caldav", caldav_todo_add, caldav_todo_done };

static const Backend *
backend(const Account *a)
{
	for (size_t i = 0; i < LENGTH(backends); i++)
		if (!strcmp(a->provider, backends[i].provider))
			return &backends[i];
	return &caldav;
}

static Account *
account_find(Vault *v, const char *id)
{
	for (int i = 0; i < v->nacc; i++)
		if (!strcmp(v->acc[i].id, id))
			return &v->acc[i];
	return NULL;
}

static int
refuse(const Account *a, SyncRun *run)
{
	run_fail(run, "%s", a ? "This account is read-only" : "Account not found");
	return -1;
}

static int
local_add(Vault *v, const TodoNew *t, SyncRun *run)
{
	LocalTodo *lt;
	uint32_t r;

	if (v->ntodo >= TODO_MAX) {
		run_fail(run, "Too many local to-dos");
		return -1;
	}
	if (getrandom(&r, sizeof r, 0) != (ssize_t)sizeof r) {
		run_fail(run, "No secure random source");
		return -1;
	}
	lt = &v->todo[v->ntodo++];
	*lt = (LocalTodo){ .y = t->y, .m = t->m, .d = t->d };
	snprintf(lt->id, sizeof lt->id, "%08" PRIx32, r);
	memcpy(lt->title, t->title, sizeof lt->title);
	return 0;
}

static int
local_done(Vault *v, const char *id, SyncRun *run)
{
	for (int i = 0; i < v->ntodo; i++) {
		if (strcmp(v->todo[i].id, id))
			continue;
		v->todo[i] = v->todo[--v->ntodo];
		return 0;
	}
	run_fail(run, "To-do not found");
	return -1;
}

int
todo_add(Vault *v, const char *target, const TodoNew *t, SyncRun *run)
{
	Account *a;

	if (!strcmp(target, LOCAL_ID))
		return local_add(v, t, run);
	a = account_find(v, target);
	if (!a || !backend(a)->add)
		return refuse(a, run);
	return backend(a)->add(a, t, run);
}

int
todo_done(Vault *v, const char *ref, SyncRun *run)
{
	size_t n = strcspn(ref, "|");
	char id[sizeof v->acc->id];
	Account *a;

	if (!ref[n] || n >= sizeof id) {
		run_fail(run, "To-do not found");
		return -1;
	}
	memcpy(id, ref, n);
	id[n] = 0;
	if (!strcmp(id, LOCAL_ID))
		return local_done(v, ref + n + 1, run);
	a = account_find(v, id);
	if (!a || !backend(a)->done)
		return refuse(a, run);
	return backend(a)->done(a, ref + n + 1, run);
}

static void
local_event(const LocalTodo *t, CalEvent *e)
{
	e->flags = EV_TASK;
	e->color = LOCAL_COLOR;
	snprintf(e->title, sizeof e->title, "%s", t->title[0] ? t->title : NO_TITLE);
	snprintf(e->cal, sizeof e->cal, "%s", LOCAL_CAL);
	snprintf(e->ref, sizeof e->ref, "%s|%s", LOCAL_ID, t->id);
	if (!t->y)
		return;
	e->flags |= EV_ALLDAY;
	e->start = local_midnight(t->y, t->m, t->d);
	e->end = local_midnight(t->y, t->m, t->d + 1);
}

void
local_todos(const Vault *v, EventList *out)
{
	for (int i = 0; i < v->ntodo; i++)
		local_event(&v->todo[i], events_push(out));
}
