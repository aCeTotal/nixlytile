/* Calendar sync helper entry point. */
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include "calsync.h"

#define NICE_LEVEL         10
#define IOPRIO_WHO_PROCESS 1
#define IOPRIO_IDLE        (3 << 13)
#define PRIVATE_UMASK      077
#define NOT_DUMPABLE       0UL
#define OOM_ADJ_PATH       "/proc/self/oom_score_adj"
#define OOM_ADJ_FIRST      "1000"
#define MESSAGE_MAX        160
#define TITLE_IN_MAX       1024
#define DUE_MAX            16
#define DUE_FIELDS         3
#define MONTHS             12
#define MONTH_DAYS_MAX     31
#define ARGC_PLAIN         2
#define ARGC_WITH_ARG      3
#define LOCAL_TARGET       "local"
#define NO_STORAGE         "Secure storage unavailable"
#define USAGE              "Usage: nixly-calsync sync|add <provider>|remove <id>|todo-add|todo-done"
#define LENGTH(a)          (sizeof (a) / sizeof *(a))
#define KEY(T, k, f)       { k, offsetof(T, f), sizeof ((T *)0)->f }

/* CMD_ADD onward take arguments. */
typedef enum {
	CMD_SYNC, CMD_TODO_ADD, CMD_TODO_DONE, CMD_ADD, CMD_REMOVE, CMD_NONE
} Command;

typedef struct {
	char target[sizeof ((Account *)0)->id];
	char title[TITLE_IN_MAX];
	char due[DUE_MAX];
	char ref[sizeof ((CalEvent *)0)->ref];
} TodoInput;

static const char *const command_names[] = {
	[CMD_SYNC] = "sync", [CMD_TODO_ADD] = "todo-add", [CMD_TODO_DONE] = "todo-done",
	[CMD_ADD] = "add", [CMD_REMOVE] = "remove",
};

static const FormKey account_keys[] = {
	KEY(Account, "url", url),
	KEY(Account, "user", user),
	KEY(Account, "pass", secret),
	KEY(Account, "client_id", client_id),
	KEY(Account, "client_secret", client_secret),
};

static const FormKey todo_keys[] = {
	KEY(TodoInput, "target", target),
	KEY(TodoInput, "title", title),
	KEY(TodoInput, "due", due),
	KEY(TodoInput, "ref", ref),
};

static Vault vault, snapshot;
static int edit_lock = -1;

void
say(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	putchar('\n');
	fflush(stdout);
}

void *
xrealloc(void *p, size_t n)
{
	void *q = realloc(p, n);

	if (q)
		return q;
	say("error Out of memory");
	exit(EXIT_FAILURE);
}

/* Messages stay one protocol line. */
void
run_fail(SyncRun *run, const char *fmt, ...)
{
	char raw[sizeof run->error];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(raw, sizeof raw, fmt, ap);
	va_end(ap);
	event_text(run->error, MESSAGE_MAX + 1, raw);
}

/* Run first: /proc locks later. */
static void
yield_memory(void)
{
	int fd = open(OOM_ADJ_PATH, O_WRONLY | O_CLOEXEC);

	if (fd < 0)
		return;
	write_all(fd, OOM_ADJ_FIRST, sizeof OOM_ADJ_FIRST - 1);
	close(fd);
}

/* Crashes must not dump secrets. */
static void
forbid_dumps(void)
{
	setrlimit(RLIMIT_CORE, &(struct rlimit){ 0, 0 });
	prctl(PR_SET_DUMPABLE, NOT_DUMPABLE);
}

static const char *
reason(const SyncRun *run)
{
	return run->error[0] ? run->error : "Request failed";
}

static void
cmd_add(const char *provider, SyncRun *run)
{
	Account draft = { 0 };
	Form f = { account_keys, LENGTH(account_keys), &draft };
	int i = -1;

	snprintf(draft.provider, sizeof draft.provider, "%s", provider);
	if (form_read(&f))
		run_fail(run, "Invalid input");
	else
		i = account_add(&vault, &draft, run);
	explicit_bzero(&draft, sizeof draft);
	if (i < 0) {
		say("error %s", reason(run));
		return;
	}
	say("added %s\t%s", vault.acc[i].id, vault.acc[i].label);
	/* Second pass re-touches its sources. */
	run->ntouched = 0;
	sync_accounts(&vault, NULL, run);
}

/* Empty or YYYY-MM-DD. */
static int
parse_due(const char *s, TodoNew *t)
{
	int end = 0;

	if (!*s)
		return 0;
	if (sscanf(s, "%4d-%2d-%2d%n", &t->y, &t->m, &t->d, &end) != DUE_FIELDS || s[end])
		return -1;
	return t->y > 0 && t->m >= 1 && t->m <= MONTHS && t->d >= 1
			&& t->d <= MONTH_DAYS_MAX ? 0 : -1;
}

static int
edit_begin(void)
{
	edit_lock = vault_acquire(&vault);
	if (edit_lock < 0) {
		say("error " NO_STORAGE);
		return -1;
	}
	memcpy(&snapshot, &vault, sizeof vault);
	return 0;
}

/* Local merges finish before unlocking. */
static void
edit_end(int rc, const char *target, SyncRun *run)
{
	int lost = memcmp(&snapshot, &vault, sizeof vault) && vault_save(&vault);

	if (lost && !rc)
		run_fail(run, NO_STORAGE);
	if (rc || lost) {
		vault_unlock(edit_lock);
		say("error %s", reason(run));
		return;
	}
	say("todo ok");
	if (!strcmp(target, LOCAL_TARGET)) {
		sync_merge(&vault, &run->w);
		vault_unlock(edit_lock);
		return;
	}
	vault_unlock(edit_lock);
	sync_accounts(&vault, target, run);
}

/* Merge before unlocking, like edits. */
static void
cmd_remove(const char *id, SyncRun *run)
{
	if (edit_begin())
		return;
	if (account_remove(&vault, id, run)) {
		vault_unlock(edit_lock);
		say("error %s", reason(run));
		return;
	}
	say("removed %s", id);
	sync_merge(&vault, &run->w);
	vault_unlock(edit_lock);
}

static void
cmd_todo_add(SyncRun *run)
{
	TodoInput in = { 0 };
	TodoNew t = { 0 };
	Form f = { todo_keys, LENGTH(todo_keys), &in };

	if (form_read(&f) || parse_due(in.due, &t)) {
		say("error Invalid input");
		return;
	}
	event_text(t.title, sizeof t.title, in.title);
	if (!edit_begin())
		edit_end(todo_add(&vault, in.target, &t, run), in.target, run);
}

static void
cmd_todo_done(SyncRun *run)
{
	TodoInput in = { 0 };
	Form f = { todo_keys, LENGTH(todo_keys), &in };

	if (form_read(&f)) {
		say("error Invalid input");
		return;
	}
	snprintf(in.target, sizeof in.target, "%.*s", (int)strcspn(in.ref, "|"), in.ref);
	if (!edit_begin())
		edit_end(todo_done(&vault, in.ref, run), in.target, run);
}

static Command
command(int argc, char **argv)
{
	for (int i = 0; argc > 1 && i < CMD_NONE; i++) {
		if (strcmp(argv[1], command_names[i]))
			continue;
		return argc == (i < CMD_ADD ? ARGC_PLAIN : ARGC_WITH_ARG) ? (Command)i : CMD_NONE;
	}
	return CMD_NONE;
}

int
main(int argc, char **argv)
{
	static SyncRun run;

	yield_memory();
	forbid_dumps();
	setpriority(PRIO_PROCESS, 0, NICE_LEVEL);
	syscall(SYS_ioprio_set, IOPRIO_WHO_PROCESS, 0, IOPRIO_IDLE);
	umask(PRIVATE_UMASK);
	signal(SIGPIPE, SIG_IGN);
	http_init();
	run.w = sync_window();
	switch (command(argc, argv)) {
	case CMD_SYNC:
		sync_all(&vault, &run);
		break;
	case CMD_TODO_ADD:
		cmd_todo_add(&run);
		break;
	case CMD_TODO_DONE:
		cmd_todo_done(&run);
		break;
	case CMD_ADD:
		cmd_add(argv[2], &run);
		break;
	case CMD_REMOVE:
		cmd_remove(argv[2], &run);
		break;
	case CMD_NONE:
		say("error " USAGE);
		break;
	}
	http_cleanup();
	return 0;
}
