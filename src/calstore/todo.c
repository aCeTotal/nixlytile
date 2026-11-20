/* To-do jobs, shown optimistically. */
#include "nixlytile.h"
#include "calstore/calstore.h"
#include "netsys.h"

#define PENDING_MAX  16
#define DUE_LEN      16
#define TODO_PAYLOAD (sizeof("target=\ndue=\ntitle=\n") + sizeof(target) + \
		DUE_LEN + TEXT_ENTRY_MAX)

static PendingTodo pend[PENDING_MAX];
static int npend, next_token;
static char hidden[PENDING_MAX][REF_LEN];
static int nhidden;
static char target[ID_LEN] = TARGET_LOCAL;

static int
writable(const char *id)
{
	const CalAccount *a = store_account_by_id(id);

	return a && a->writable;
}

/* Keeps a vanished target valid. */
static void
target_fix(void)
{
	if (!strcmp(target, TARGET_LOCAL) || writable(target))
		return;
	snprintf(target, sizeof(target), TARGET_LOCAL);
	for (int i = store_account_count() - 1; i >= 0; i--)
		if (store_account(i)->writable)
			snprintf(target, sizeof(target), "%s",
					store_account(i)->id);
}

const char *
todo_target(void)
{
	target_fix();
	return target;
}

/* Writable accounts, then local. */
void
todo_target_next(void)
{
	int n = store_account_count(), at = n;

	target_fix();
	for (int i = 0; i < n; i++)
		if (!strcmp(store_account(i)->id, target))
			at = i;
	for (int i = at + 1; i < n; i++) {
		if (!store_account(i)->writable)
			continue;
		snprintf(target, sizeof(target), "%s", store_account(i)->id);
		return;
	}
	snprintf(target, sizeof(target), "%s", at == n ? "" : TARGET_LOCAL);
	target_fix();
}

void
todo_add(const char *title, int day)
{
	char payload[TODO_PAYLOAD], due[DUE_LEN];
	Civil c = civil_from_day(day);
	PendingTodo *p = &pend[npend];
	int token = next_token + 1;
	int n;

	if (npend == PENDING_MAX || !title[0] || strpbrk(title, "\r\n"))
		return;
	snprintf(due, sizeof(due), "%04d-%02d-%02d", c.y, c.m, c.d);
	n = snprintf(payload, sizeof(payload), "target=%s\ndue=%s\ntitle=%s\n",
			todo_target(), due, title);
	if (n < 0 || (size_t)n >= sizeof(payload))
		return;
	if (job_spawn(&(JobSpec){ .kind = JOB_TODO, .cmd = "todo-add",
			.payload = payload, .token = token }) < 0)
		return;
	next_token = token;
	p->token = token;
	p->day = day;
	snprintf(p->title, sizeof(p->title), "%s", title);
	npend++;
}

void
todo_done(const char *ref)
{
	char payload[REF_LEN + sizeof("ref=\n")];

	if (nhidden == PENDING_MAX || todo_hidden(ref))
		return;
	snprintf(payload, sizeof(payload), "ref=%s\n", ref);
	if (job_spawn(&(JobSpec){ .kind = JOB_TODO, .cmd = "todo-done",
			.payload = payload, .ref = ref }) < 0)
		return;
	snprintf(hidden[nhidden++], REF_LEN, "%s", ref);
}

static void
drop_pending(int token)
{
	for (int i = 0; i < npend; i++) {
		if (pend[i].token != token)
			continue;
		pend[i] = pend[--npend];
		return;
	}
}

static void
drop_hidden(const char *ref)
{
	for (int i = 0; i < nhidden; i++) {
		if (strcmp(hidden[i], ref))
			continue;
		memcpy(hidden[i], hidden[--nhidden], REF_LEN);
		return;
	}
}

void
todo_finished(int token, const char *ref, const char *error)
{
	char msg[MSG_LEN];

	snprintf(msg, sizeof(msg), "To-do not saved · %s", error);
	if (error[0])
		osd_show(selmon, msg);
	if (token)
		drop_pending(token);
	if (ref[0])
		drop_hidden(ref);
}

int
todo_pending_count(void)
{
	return npend;
}

const PendingTodo *
todo_pending(int i)
{
	return &pend[i];
}

int
todo_hidden(const char *ref)
{
	for (int i = 0; i < nhidden; i++)
		if (!strcmp(hidden[i], ref))
			return 1;
	return 0;
}
