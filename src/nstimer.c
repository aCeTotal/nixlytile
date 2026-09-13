#include "nixlytile.h"
#include <sys/timerfd.h>

/*
 * Nanosecond-precision one-shot timers on the compositor event loop.
 *
 * wl_event_loop timers are ms-granular (wl_event_source_timer_update takes
 * whole milliseconds), which quantizes sub-frame scheduling: at 144 Hz a
 * 6.94 ms frame gets up to 1 ms of pacing jitter from the timer API alone.
 * This wraps a raw CLOCK_MONOTONIC timerfd armed with TFD_TIMER_ABSTIME so
 * pace.c/latch.c deadlines land where they were computed (absolute ns on
 * the same clock as get_time_ns()).
 */

struct NsTimer {
	int fd;
	struct wl_event_source *src;
	int (*cb)(void *);
	void *data;
};

static int
nstimer_dispatch(int fd, uint32_t mask, void *data)
{
	NsTimer *t = data;
	uint64_t expirations;

	(void)mask;
	while (read(fd, &expirations, sizeof(expirations)) > 0)
		;
	return t->cb(t->data);
}

NsTimer *
nstimer_create(int (*cb)(void *), void *data)
{
	NsTimer *t;
	int fd;

	fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
	if (fd < 0)
		return NULL;
	t = calloc(1, sizeof(*t));
	if (!t) {
		close(fd);
		return NULL;
	}
	t->fd = fd;
	t->cb = cb;
	t->data = data;
	t->src = wl_event_loop_add_fd(event_loop, fd, WL_EVENT_READABLE,
			nstimer_dispatch, t);
	if (!t->src) {
		close(fd);
		free(t);
		return NULL;
	}
	return t;
}

/* One-shot absolute deadline in CLOCK_MONOTONIC ns (get_time_ns() base).
 * A deadline in the past fires immediately. */
void
nstimer_arm_abs(NsTimer *t, uint64_t abs_ns)
{
	struct itimerspec its = {0};

	if (!t)
		return;
	/* value 0 would disarm; clamp to 1 ns for "fire now". */
	if (abs_ns == 0)
		abs_ns = 1;
	its.it_value.tv_sec = abs_ns / 1000000000ULL;
	its.it_value.tv_nsec = abs_ns % 1000000000ULL;
	timerfd_settime(t->fd, TFD_TIMER_ABSTIME, &its, NULL);
}

void
nstimer_disarm(NsTimer *t)
{
	struct itimerspec its = {0};

	if (!t)
		return;
	timerfd_settime(t->fd, 0, &its, NULL);
}

void
nstimer_destroy(NsTimer *t)
{
	if (!t)
		return;
	wl_event_source_remove(t->src);
	close(t->fd);
	free(t);
}
