#include <pthread.h>

#include "nixlytile.h"
#include "launchboost.h"

/* Covers exec through first paint. */
#define LB_BOOST_MS 2500

static pthread_mutex_t lb_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  lb_cond = PTHREAD_COND_INITIALIZER;
static uint64_t lb_deadline_ms;   /* 0 = no boost requested */
static int lb_boosted;
static int lb_running;
static struct wl_listener lb_client_created;

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
				/* Later kicks extend the sleep. */
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

		/* Restore the power-state cap. */
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

/* Covers launches from terminals too. */
static void
client_created(struct wl_listener *listener, void *data)
{
	(void)listener;
	(void)data;
	launchboost_kick();
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
	lb_client_created.notify = client_created;
	wl_display_add_client_created_listener(dpy, &lb_client_created);
}

void
launchboost_kick(void)
{
	if (!lb_running)
		return;
	/* Uncapped already on wall power. */
	if (powersave_clock_cap() >= 1.0)
		return;

	pthread_mutex_lock(&lb_lock);
	lb_deadline_ms = monotonic_msec() + LB_BOOST_MS;
	pthread_cond_signal(&lb_cond);
	pthread_mutex_unlock(&lb_lock);
}
