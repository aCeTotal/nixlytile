#include "layer.h"

#include <signal.h>
#include <stdio.h>

#define WORKER_NAME "nixly-vrs"

/* Faults must reach handlers. */
static const int synchronous[] = { SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGSYS };

/* Sleeps until kicked; 0: quit. */
static int
await(struct worker *w)
{
	pthread_mutex_lock(&w->lock);
	while (!w->kicked && !atomic_load(&w->quit))
		pthread_cond_wait(&w->wake, &w->lock);
	w->kicked = 0;
	pthread_mutex_unlock(&w->lock);
	return !atomic_load(&w->quit);
}

static void *
work(void *arg)
{
	struct device *dev = arg;

	while (await(&dev->fovea.worker)) {
		upload_create(dev);
		rate_build(dev);
	}
	return NULL;
}

void
worker_kick(struct worker *w)
{
	pthread_mutex_lock(&w->lock);
	w->kicked = 1;
	pthread_cond_signal(&w->wake);
	pthread_mutex_unlock(&w->lock);
}

/* Async app signals never reach it. */
void
worker_start(struct device *dev)
{
	struct worker *w = &dev->fovea.worker;
	sigset_t all, app;
	size_t i;

	pthread_mutex_init(&w->lock, NULL);
	pthread_cond_init(&w->wake, NULL);
	if (!dev->vrs)
		return;
	sigfillset(&all);
	for (i = 0; i < LENGTH(synchronous); i++)
		sigdelset(&all, synchronous[i]);
	pthread_sigmask(SIG_SETMASK, &all, &app);
	w->running = pthread_create(&w->thread, NULL, work, dev) == 0;
	pthread_sigmask(SIG_SETMASK, &app, NULL);
	if (!w->running) {
		fprintf(stderr, "nixly-vrs: no rate worker, rates native\n");
		return;
	}
	pthread_setname_np(w->thread, WORKER_NAME);
}

/* Waits out in-flight creation. */
void
worker_stop(struct device *dev)
{
	struct worker *w = &dev->fovea.worker;

	if (w->running) {
		atomic_store(&w->quit, 1);
		worker_kick(w);
		pthread_join(w->thread, NULL);
	}
	pthread_cond_destroy(&w->wake);
	pthread_mutex_destroy(&w->lock);
}
