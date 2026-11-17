/* Terminal context, read off-thread. */
#include "overview/state.h"

typedef struct {
	Client *c; /* compared only, never off-thread */
	pid_t pid;
	TiInfo info;
} Probe;

typedef struct {
	uint64_t epoch;
	int n;
	Probe probe[];
} Job;

static int done_pipe[2] = { -1, -1 };
static uint64_t epoch;

static void *
probe_all(void *arg)
{
	Job *job = arg;

	for (int i = 0; i < job->n; i++)
		terminfo_collect(job->probe[i].pid, &job->probe[i].info);
	if (write(done_pipe[1], &job, sizeof(job)) != sizeof(job))
		free(job);
	return NULL;
}

static Cell *
cell_of(Client *c)
{
	for (int i = 0; i < ov.n_cells; i++)
		if (ov.cells[i].c == c)
			return &ov.cells[i];
	return NULL;
}

static void
apply(const Probe *p)
{
	Cell *cell = cell_of(p->c);

	if (!cell || !p->info.text[0])
		return;
	cell->kind = p->info.kind == TI_SSH ? DETAIL_REMOTE : DETAIL_PATH;
	snprintf(cell->detail, sizeof(cell->detail),
			p->info.kind == TI_SSH ? "ssh %s" : "%s", p->info.text);
	cell_relabel(cell);
}

static void
show(const Job *job)
{
	for (int i = 0; i < job->n; i++)
		apply(&job->probe[i]);
	overview_redraw();
}

static int
delivered(int fd, uint32_t mask, void *data)
{
	Job *job;

	while (read(fd, &job, sizeof(job)) == sizeof(job)) {
		if (job->epoch == epoch)
			show(job);
		free(job);
	}
	return 0;
}

static int
pipe_ready(void)
{
	if (done_pipe[0] >= 0)
		return 1;
	if (pipe2(done_pipe, O_CLOEXEC) < 0)
		return 0;
	fcntl(done_pipe[0], F_SETFL, O_NONBLOCK);
	wl_event_loop_add_fd(event_loop, done_pipe[0], WL_EVENT_READABLE,
			delivered, NULL);
	return 1;
}

/* Probe every terminal cell. */
void
detail_fetch(void)
{
	Job *job;
	pthread_t tid;

	if (!pipe_ready())
		return;
	job = ecalloc(1, sizeof(*job) + (size_t)ov.n_cells * sizeof(Probe));
	job->epoch = ++epoch;
	for (int i = 0; i < ov.n_cells; i++)
		if (is_terminal_client(ov.cells[i].c))
			job->probe[job->n++] = (Probe){ .c = ov.cells[i].c,
					.pid = client_get_pid(ov.cells[i].c) };
	if (!job->n || pthread_create(&tid, NULL, probe_all, job)) {
		free(job);
		return;
	}
	pthread_detach(tid);
}

/* In-flight results go stale. */
void
detail_drop(void)
{
	epoch++;
}
