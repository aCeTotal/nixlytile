/*
 * launchboost.c — full clocks for the first seconds of an app launch.
 * See launchboost.h.
 */
#include <pthread.h>

#include "nixlytile.h"
#include "launchboost.h"

/* Long enough to cover exec + dynamic linking + toolkit init + first
 * paint (measured: ~550ms for Thunar, ~180ms for Alacritty on AC), with
 * room for a cold binary.  Short enough that a mistimed kick costs a
 * negligible amount of battery. */
#define LB_BOOST_MS 2500

static pthread_mutex_t lb_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  lb_cond = PTHREAD_COND_INITIALIZER;
static uint64_t lb_deadline_ms;   /* 0 = no boost requested */
static int lb_boosted;
static int lb_running;

static void *
lb_worker(void *arg)
{
	(void)arg;

	pthread_mutex_lock(&lb_lock);
	for (;;) {
		uint64_t now, deadline;

		while (!lb_deadline_ms && !lb_boosted)
			pthread_cond_wait(&lb_cond, &lb_lock);

		now = monotonic_msec();
		deadline = lb_deadline_ms;

		if (deadline > now) {
			if (!lb_boosted) {
				lb_boosted = 1;
				pthread_mutex_unlock(&lb_lock);
				cpuclock_cap(1.0);
				pthread_mutex_lock(&lb_lock);
			}
			{
				/* Re-check on wake: a kick that lands while we
				 * sleep pushes lb_deadline_ms further out and
				 * the loop simply sleeps again. */
				struct timespec ts;
				uint64_t wait = deadline - now;
				clock_gettime(CLOCK_REALTIME, &ts);
				ts.tv_sec  += (time_t)(wait / 1000);
				ts.tv_nsec += (long)((wait % 1000) * 1000000L);
				if (ts.tv_nsec >= 1000000000L) {
					ts.tv_sec++;
					ts.tv_nsec -= 1000000000L;
				}
				pthread_cond_timedwait(&lb_cond, &lb_lock, &ts);
			}
			continue;
		}

		/* Window elapsed — hand the cap back to whatever the power
		 * state says it should be.  Only the cap: the governor, EPP,
		 * turbo and platform profile were never touched, so there is
		 * nothing else to undo and no way to fight powersave.c. */
		lb_deadline_ms = 0;
		if (lb_boosted) {
			lb_boosted = 0;
			pthread_mutex_unlock(&lb_lock);
			cpuclock_cap(powersave_clock_cap());
			pthread_mutex_lock(&lb_lock);
		}
	}
	pthread_mutex_unlock(&lb_lock);
	return NULL;
}

void
launchboost_init(void)
{
	pthread_t tid;
	pthread_attr_t attr;

	if (lb_running)
		return;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	if (pthread_create(&tid, &attr, lb_worker, NULL) == 0)
		lb_running = 1;
	pthread_attr_destroy(&attr);
}

void
launchboost_kick(void)
{
	if (!lb_running)
		return;
	/* Nothing to release on wall power — the cap is already 1.0, and
	 * writing it again would be ~20 pointless sysfs stores per launch. */
	if (powersave_clock_cap() >= 1.0)
		return;

	pthread_mutex_lock(&lb_lock);
	lb_deadline_ms = monotonic_msec() + LB_BOOST_MS;
	pthread_cond_signal(&lb_cond);
	pthread_mutex_unlock(&lb_lock);
}
