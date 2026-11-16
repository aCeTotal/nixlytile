#include "nixlytile.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#include <unistd.h>

#include "mailbox.h"

#define ACTIVE_MS 1000
#define LINGER_MS 60000
#define GAZE_VALID (1ull << 16)

/* Shared with the gaze thread. */
static struct {
	pthread_mutex_t lock;
	pthread_cond_t wake;
	_Atomic int run;
	_Atomic int asleep;
	_Atomic int calibrating;
	_Atomic int poked;
	_Atomic uint64_t demand_ms;
	_Atomic uint64_t sample;
	_Atomic unsigned model_gen;
	GazeModel model;        /* under lock */
	int has_model;          /* under lock */
	Features latest;        /* under lock */
	int latest_ok;          /* under lock */
	int pipe[2];
} mb = {
	.lock = PTHREAD_MUTEX_INITIALIZER,
	.wake = PTHREAD_COND_INITIALIZER,
	.pipe = { -1, -1 },
};

static void
wake(void)
{
	if (!atomic_load(&mb.asleep))
		return;
	pthread_mutex_lock(&mb.lock);
	pthread_cond_signal(&mb.wake);
	pthread_mutex_unlock(&mb.lock);
}

void
mailbox_demand(void)
{
	atomic_store_explicit(&mb.demand_ms, monotonic_msec(), memory_order_relaxed);
	wake();
}

void
mailbox_calibrate(int on)
{
	atomic_store(&mb.calibrating, on);
	wake();
}

void
mailbox_model(const GazeModel *m)
{
	pthread_mutex_lock(&mb.lock);
	mb.has_model = m != NULL;
	if (m)
		mb.model = *m;
	pthread_mutex_unlock(&mb.lock);
	atomic_fetch_add(&mb.model_gen, 1);
}

GazeSample
mailbox_sample(void)
{
	uint64_t v = atomic_load_explicit(&mb.sample, memory_order_relaxed);

	return (GazeSample){ (float)(v & 0xffff) / GAZE_UNIT, (v & GAZE_VALID) != 0 };
}

int
mailbox_features(Features *f)
{
	int ok;

	pthread_mutex_lock(&mb.lock);
	*f = mb.latest;
	ok = mb.latest_ok;
	pthread_mutex_unlock(&mb.lock);
	return ok;
}

int
mailbox_fd(void)
{
	return mb.pipe[0];
}

void
mailbox_drain(void)
{
	char buf[16];

	atomic_store(&mb.poked, 0);
	while (read(mb.pipe[0], buf, sizeof(buf)) > 0)
		;
}

int
mailbox_open(void)
{
	if (pipe2(mb.pipe, O_CLOEXEC | O_NONBLOCK) < 0)
		return -1;
	atomic_store(&mb.run, 1);
	return 0;
}

/* Signals exit; 0 when absent. */
int
mailbox_stop(void)
{
	if (!atomic_exchange(&mb.run, 0))
		return 0;
	pthread_mutex_lock(&mb.lock);
	pthread_cond_broadcast(&mb.wake);
	pthread_mutex_unlock(&mb.lock);
	return 1;
}

void
mailbox_close(void)
{
	close(mb.pipe[0]);
	close(mb.pipe[1]);
	mb.pipe[0] = mb.pipe[1] = -1;
}

/* Full rate: demanded or calibrating. */
int
mailbox_active(void)
{
	return atomic_load(&mb.calibrating) ||
		monotonic_msec() - atomic_load(&mb.demand_ms) < ACTIVE_MS;
}

/* Lingers for instant game return. */
int
mailbox_wanted(void)
{
	return atomic_load(&mb.run) && (atomic_load(&mb.calibrating) ||
		monotonic_msec() - atomic_load(&mb.demand_ms) < LINGER_MS);
}

/* Waits for demand or stop. */
int
mailbox_await(void)
{
	int run;

	pthread_mutex_lock(&mb.lock);
	atomic_store(&mb.asleep, 1);
	while (atomic_load(&mb.run) && !mailbox_wanted())
		pthread_cond_wait(&mb.wake, &mb.lock);
	atomic_store(&mb.asleep, 0);
	run = atomic_load(&mb.run);
	pthread_mutex_unlock(&mb.lock);
	return run;
}

void
mailbox_pause(long ms)
{
	struct timespec until;

	clock_gettime(CLOCK_REALTIME, &until);
	until.tv_sec += ms / 1000;
	until.tv_nsec += ms % 1000 * 1000000;
	until.tv_sec += until.tv_nsec / 1000000000;
	until.tv_nsec %= 1000000000;
	pthread_mutex_lock(&mb.lock);
	if (atomic_load(&mb.run))
		pthread_cond_timedwait(&mb.wake, &mb.lock, &until);
	pthread_mutex_unlock(&mb.lock);
}

unsigned
mailbox_model_gen(void)
{
	return atomic_load(&mb.model_gen);
}

/* Copies model; 0 when none. */
int
mailbox_copy_model(GazeModel *m)
{
	int has;

	pthread_mutex_lock(&mb.lock);
	*m = mb.model;
	has = mb.has_model;
	pthread_mutex_unlock(&mb.lock);
	return has;
}

int
mailbox_calibrating(void)
{
	return atomic_load(&mb.calibrating);
}

void
mailbox_hand_over(const Features *f, int ok)
{
	pthread_mutex_lock(&mb.lock);
	if (ok)
		mb.latest = *f;
	mb.latest_ok = ok;
	pthread_mutex_unlock(&mb.lock);
	if (!atomic_exchange(&mb.poked, 1))
		(void)!write(mb.pipe[1], "g", 1);
}

void
mailbox_publish(float x)
{
	x = x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x;
	atomic_store(&mb.sample, (uint64_t)(uint16_t)(x * GAZE_UNIT) | GAZE_VALID);
}

void
mailbox_clear(void)
{
	atomic_store(&mb.sample, 0);
}
