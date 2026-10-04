#include "nixlytile.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "pwaudible.h"
#include "util.h"

#define PWA_MAX_PIDS  128
#define PWA_CHUNK     65536u
#define PWA_DUMP_MAX  (8u << 20)
#define PWA_PID_KEY   "\"application.process.id\":"

static pid_t pids[PWA_MAX_PIDS];
static int npids;
static uint64_t taken_at;

static pid_t dump_pid = -1;
static int dump_fd = -1;
static struct wl_event_source *dump_src;
static char *buf;
static size_t len, cap;
static void (*on_done)(void);

static void
dump_close(void)
{
	if (dump_src)
		wl_event_source_remove(dump_src);
	if (dump_fd >= 0)
		close(dump_fd);
	dump_src = NULL;
	dump_fd = -1;
	dump_pid = -1;
}

static int
grow(void)
{
	char *bigger;
	size_t want = cap ? cap * 2 : PWA_CHUNK;

	if (cap - len >= PWA_CHUNK / 16)
		return 1;
	if (want > PWA_DUMP_MAX)
		return 0;
	bigger = realloc(buf, want);
	if (!bigger)
		return 0;
	buf = bigger;
	cap = want;
	return 1;
}

/* Node type, then state, then pid. */
static int
parse(void)
{
	char *line, *save = NULL;
	int node = 0, running = 0, valid = 0;

	npids = 0;
	for (line = strtok_r(buf, "\n", &save); line;
			line = strtok_r(NULL, "\n", &save)) {
		char *key;

		if (strstr(line, "\"type\": \"PipeWire:Interface:")) {
			valid = 1;
			node = strstr(line, "Interface:Node\"") != NULL;
			running = 0;
			continue;
		}
		if (node && strstr(line, "\"state\": \"running\"")) {
			running = 1;
			continue;
		}
		key = strstr(line, PWA_PID_KEY);
		if (!key || !running || npids >= PWA_MAX_PIDS)
			continue;
		pids[npids++] = (pid_t)atoi(key + strlen(PWA_PID_KEY));
	}
	return valid;
}

static int
readable(int fd, uint32_t mask, void *data)
{
	ssize_t n = 0;
	int whole = 1;

	for (;;) {
		if (!grow()) {
			whole = 0;
			break;
		}
		n = read(fd, buf + len, cap - len - 1);
		if (n <= 0)
			break;
		len += (size_t)n;
	}
	if (whole && n < 0 && errno == EAGAIN)
		return 0;

	dump_close();
	buf[len] = '\0';
	if (whole && parse())
		taken_at = monotonic_msec();
	on_done();
	return 0;
}

void
pwaudible_query(void (*done)(void))
{
	const char *const argv[] = { "pw-dump", "--no-colors", NULL };

	if (dump_pid > 0)
		return;
	if (spawn_argv_read(argv, &dump_pid, &dump_fd) != 0)
		return;
	on_done = done;
	len = 0;
	dump_src = wl_event_loop_add_fd(event_loop, dump_fd,
		WL_EVENT_READABLE, readable, NULL);
	if (!dump_src)
		dump_close();
}

int
pwaudible_has(pid_t pid)
{
	int i;

	for (i = 0; i < npids; i++) {
		if (pids[i] == pid)
			return 1;
	}
	return 0;
}

uint64_t
pwaudible_at(void)
{
	return taken_at;
}
