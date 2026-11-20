/* Helper jobs and their output. */
#include "nixlytile.h"
#include "calstore/calstore.h"

#include <errno.h>
#include <signal.h>

#define JOBS_MAX 6
#define JOB_LINE 5120
#define CHUNK    512
#define MISSING  "Calendar helper nixly-calsync is missing"

typedef struct {
	JobKind kind;
	int token;
	char ref[REF_LEN];
	int added, done;
	char error[ERROR_LEN];
} JobResult;

typedef struct {
	int used;
	pid_t pid;
	int fd;
	struct wl_event_source *ev;
	char line[JOB_LINE];
	size_t len;
	JobResult res;
} Job;

typedef struct {
	const char *prefix;
	void (*fn)(Job *j, char *args);
} LineRule;

static Job jobs[JOBS_MAX];

static void
on_acct(Job *j, char *args)
{
	sync_account_line(args);
}

/* Remaining output is sync. */
static void
on_added(Job *j, char *args)
{
	char *label = strchr(args, '\t');

	j->res.added = 1;
	j->res.kind = JOB_SYNC;
	calendar_account_added(label ? label + 1 : args);
}

static void
on_error(Job *j, char *args)
{
	snprintf(j->res.error, sizeof(j->res.error), "%s", args);
}

static void
on_done(Job *j, char *args)
{
	j->res.done = 1;
	if (strcmp(args, "1"))
		return;
	store_load();
	calendar_changed();
}

/* Browser runs at normal priority. */
static void
on_open(Job *j, char *args)
{
	const char *argv[] = { "xdg-open", args, NULL };

	spawn_cmd_async(argv);
}

static const LineRule line_rules[] = {
	{ "acct ", on_acct },
	{ "added ", on_added },
	{ "error ", on_error },
	{ "done ", on_done },
	{ "open ", on_open },
};

static void
on_line(Job *j, char *line)
{
	for (size_t i = 0; i < LENGTH(line_rules); i++) {
		size_t n = strlen(line_rules[i].prefix);

		if (!strncmp(line, line_rules[i].prefix, n)) {
			line_rules[i].fn(j, line + n);
			return;
		}
	}
}

static void
report(const JobResult *r)
{
	char msg[MSG_LEN];

	switch (r->kind) {
	case JOB_SYNC:
		sync_finished(r->error[0] || r->done ? r->error :
				"Sync interrupted");
		break;
	case JOB_ADD:
		calendar_add_failed(r->error[0] ? r->error : "Cancelled");
		break;
	case JOB_REMOVE:
		snprintf(msg, sizeof(msg), "Account not removed · %s", r->error);
		if (r->error[0])
			osd_show(selmon, msg);
		break;
	case JOB_TODO:
		todo_finished(r->token, r->ref, r->error);
		break;
	}
}

static Job *
free_job(void)
{
	for (int i = 0; i < JOBS_MAX; i++)
		if (!jobs[i].used)
			return &jobs[i];
	return NULL;
}

static void
feed(Job *j, const char *chunk, ssize_t n)
{
	for (ssize_t i = 0; i < n; i++) {
		if (chunk[i] == '\n') {
			j->line[j->len] = '\0';
			on_line(j, j->line);
			j->len = 0;
			continue;
		}
		if (j->len < sizeof(j->line) - 1)
			j->line[j->len++] = chunk[i];
	}
}

static void start_queued(void);

static void
job_finish(Job *j)
{
	JobResult res = j->res;

	wl_event_source_remove(j->ev);
	close(j->fd);
	memset(j, 0, sizeof(*j));
	report(&res);
	start_queued();
	calendar_changed();
}

static int
job_readable(int fd, uint32_t mask, void *data)
{
	Job *j = data;
	char chunk[CHUNK];
	ssize_t n;

	while ((n = read(fd, chunk, sizeof(chunk))) > 0)
		feed(j, chunk, n);
	if (n == 0 || (errno != EAGAIN && errno != EINTR))
		job_finish(j);
	return 0;
}

static int
job_start(Job *j, const JobSpec *s)
{
	Launch l = launch_calsync(s);

	if (l.pid <= 0)
		return -1;
	fd_set_nonblock(l.fd);
	j->ev = wl_event_loop_add_fd(event_loop, l.fd, WL_EVENT_READABLE,
			job_readable, j);
	if (!j->ev) {
		kill(l.pid, SIGTERM);
		close(l.fd);
		return -1;
	}
	j->used = 1;
	j->pid = l.pid;
	j->fd = l.fd;
	j->res = (JobResult){ .kind = s->kind, .token = s->token };
	snprintf(j->res.ref, sizeof(j->res.ref), "%s", s->ref ? s->ref : "");
	return 0;
}

static void
spawn_failed(const JobSpec *s)
{
	JobResult r = { .kind = s->kind, .token = s->token };

	snprintf(r.ref, sizeof(r.ref), "%s", s->ref ? s->ref : "");
	snprintf(r.error, sizeof(r.error), MISSING);
	if (s->kind != JOB_SYNC)
		osd_show(selmon, MISSING);
	report(&r);
}

static void
start_queued(void)
{
	const JobSpec *s;
	Job *j;

	while ((s = queue_peek()) && (j = free_job())) {
		if (job_start(j, s) < 0)
			spawn_failed(s);
		queue_drop();
	}
}

/* Queues when every slot runs. */
int
job_spawn(const JobSpec *s)
{
	Job *j = free_job();

	if (!j)
		return queue_push(s);
	if (job_start(j, s) == 0)
		return 0;
	if (s->kind != JOB_SYNC)
		osd_show(selmon, MISSING);
	return -1;
}

int
job_busy(JobKind kind)
{
	for (int i = 0; i < JOBS_MAX; i++)
		if (jobs[i].used && jobs[i].res.kind == kind)
			return 1;
	return queue_holds(kind);
}

void
job_cancel(JobKind kind)
{
	for (int i = 0; i < JOBS_MAX; i++)
		if (jobs[i].used && jobs[i].res.kind == kind)
			kill(jobs[i].pid, SIGTERM);
}
