/*
 * fetch_async.c — run a shell command in the background and deliver its
 * full stdout to a callback on the compositor event loop.  Replaces the
 * synchronous popen() calls that used to freeze the cursor while a
 * statusbar popup was refreshing (top/ps/wpctl each block 50-500 ms).
 */
#include "nixlytile.h"
#include "fetch_async.h"
#include "util.h"

#include <errno.h>
#include <signal.h>
#include <sys/wait.h>

#define FETCH_SLOTS 8
#define FETCH_MAX   (256 * 1024)

typedef struct {
	int used;
	int fd;
	pid_t pid;
	struct wl_event_source *ev;
	char *buf;
	size_t len;
	fetch_done_fn done;
	void *data;
} FetchSlot;

static FetchSlot slots[FETCH_SLOTS];

static void
slot_finish(FetchSlot *s)
{
	fetch_done_fn done = s->done;
	void *data = s->data;
	char *buf = s->buf;
	size_t len = s->len;

	if (s->ev)
		wl_event_source_remove(s->ev);
	if (s->fd >= 0)
		close(s->fd);
	if (s->pid > 0)
		waitpid(s->pid, NULL, WNOHANG);
	memset(s, 0, sizeof(*s));

	if (buf)
		buf[len] = '\0';
	if (done)
		done(buf ? buf : "", buf ? len : 0, data);
	free(buf);
}

static int
fetch_cb(int fd, uint32_t mask, void *data)
{
	FetchSlot *s = data;
	char chunk[4096];
	ssize_t n;

	while ((n = read(fd, chunk, sizeof(chunk))) > 0) {
		if (s->len + (size_t)n < FETCH_MAX) {
			char *nb = realloc(s->buf, s->len + (size_t)n + 1);

			if (nb) {
				memcpy(nb + s->len, chunk, (size_t)n);
				s->buf = nb;
				s->len += (size_t)n;
			}
		}
		/* over the cap: keep draining so the child can exit */
	}
	if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR) ||
			(n < 0 && (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR))))
		slot_finish(s);
	return 0;
}

int
fetch_async(const char *cmd, fetch_done_fn done, void *data)
{
	FetchSlot *s = NULL;

	for (int i = 0; i < FETCH_SLOTS; i++) {
		if (!slots[i].used) {
			s = &slots[i];
			break;
		}
	}
	if (!s || !cmd)
		return -1;

	memset(s, 0, sizeof(*s));
	s->fd = -1;
	s->pid = -1;
	if (spawn_async_read(cmd, &s->pid, &s->fd) != 0) {
		s->fd = -1;
		s->pid = -1;
		return -1;
	}
	s->used = 1;
	s->done = done;
	s->data = data;
	s->ev = wl_event_loop_add_fd(event_loop, s->fd,
			WL_EVENT_READABLE | WL_EVENT_HANGUP, fetch_cb, s);
	if (!s->ev) {
		close(s->fd);
		kill(s->pid, SIGTERM);
		waitpid(s->pid, NULL, WNOHANG);
		memset(s, 0, sizeof(*s));
		return -1;
	}
	return 0;
}
