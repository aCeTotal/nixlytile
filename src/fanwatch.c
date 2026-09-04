/*
 * fanwatch.c — every fan sysfs read and write happens on this thread,
 * never on the compositor thread.
 *
 * Why: on MSI laptops the fan module reads /sys/devices/platform/msi-ec,
 * the same embedded controller whose battery status read measured 111ms
 * (see battwatch.c).  fan_refresh() costs five of those transactions,
 * and render_fan_popup() called it synchronously — at 500ms cadence
 * while the popup is hovered, plus once more every time the cursor
 * crossed a button inside the card.  So the cursor stuttered exactly
 * while the user was moving it over the fan popup.
 *
 * Here a worker thread owns the FanState, samples it on its own clock
 * and publishes into fan_pub through a mutex, poking the event loop
 * with a pipe.  Clicks queue their control write instead of performing
 * it, so the msi-ec cooler-boost toggle can't stall the cursor either.
 */
#include "nixlytile.h"
#include <pthread.h>

#define FW_IDLE_MS 2000
#define FW_FAST_MS  500
/* How long one fanwatch_poke_fast() keeps the fast cadence alive.  The
 * popup re-renders every 500ms while open, so it self-clears shortly
 * after the popup closes without needing a hide hook. */
#define FW_FAST_HOLD_MS 2000

#define FW_MAX_PENDING 8

enum { FW_CTL_FRAC, FW_CTL_AUTO, FW_CTL_BOOST, FW_CTL_CURVE };

typedef struct {
	int kind;
	int flat;
	double frac;
	int on;
	FanCurve curve;
} FwCtl;

static pthread_t fw_thread;
static pthread_mutex_t fw_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t fw_cond = PTHREAD_COND_INITIALIZER;
static FanState fw_pub;                /* guarded by fw_lock */
static FwCtl fw_pending[FW_MAX_PENDING]; /* guarded by fw_lock */
static int fw_npending;                /* guarded by fw_lock */
static int fw_run;                     /* guarded by fw_lock */
static uint64_t fw_fast_until_ms;      /* guarded by fw_lock */
static int fw_pipe[2] = { -1, -1 };
static struct wl_event_source *fw_src;

static void *
fw_worker(void *data)
{
	static FanState local;   /* worker-private; too big for the stack */

	(void)data;
	pthread_setname_np(pthread_self(), "nixly-fanwatch");

	fanconf_load();
	if (fan_scan_state(&local) <= 0) {
		/* No fans: publish the empty state once so the module hides,
		 * then this thread has nothing left to do. */
		pthread_mutex_lock(&fw_lock);
		fw_pub = local;
		pthread_mutex_unlock(&fw_lock);
		if (fw_pipe[1] >= 0)
			(void)!write(fw_pipe[1], "f", 1);
		return NULL;
	}
	fan_state_apply_saved(&local);

	for (;;) {
		FwCtl ctl[FW_MAX_PENDING];
		int nctl, changed;
		uint64_t now;
		unsigned wait_ms;

		pthread_mutex_lock(&fw_lock);
		if (!fw_run) {
			pthread_mutex_unlock(&fw_lock);
			break;
		}
		nctl = fw_npending;
		memcpy(ctl, fw_pending, sizeof(ctl));
		fw_npending = 0;
		pthread_mutex_unlock(&fw_lock);

		/* queued clicks first, so the sample below reflects them */
		for (int i = 0; i < nctl; i++) {
			switch (ctl[i].kind) {
			case FW_CTL_FRAC:
				fan_state_set_frac(&local, ctl[i].flat,
						ctl[i].frac);
				break;
			case FW_CTL_AUTO:
				fan_state_set_auto(&local, ctl[i].flat);
				break;
			case FW_CTL_BOOST:
				fan_state_set_boost(&local, ctl[i].on);
				break;
			case FW_CTL_CURVE:
				fan_state_set_curve(&local, ctl[i].flat,
						&ctl[i].curve);
				break;
			}
		}

		fan_refresh_state(&local);
		fan_state_curve_tick(&local);

		pthread_mutex_lock(&fw_lock);
		changed = memcmp(&fw_pub, &local, sizeof(local)) != 0;
		fw_pub = local;
		pthread_mutex_unlock(&fw_lock);

		if (changed && fw_pipe[1] >= 0)
			(void)!write(fw_pipe[1], "f", 1);

		now = monotonic_msec();
		pthread_mutex_lock(&fw_lock);
		wait_ms = now < fw_fast_until_ms ? FW_FAST_MS : FW_IDLE_MS;
		if (fw_run && !fw_npending) {
			struct timespec ts;

			clock_gettime(CLOCK_REALTIME, &ts);
			ts.tv_nsec += (long)(wait_ms % 1000) * 1000000L;
			ts.tv_sec += wait_ms / 1000 + ts.tv_nsec / 1000000000L;
			ts.tv_nsec %= 1000000000L;
			pthread_cond_timedwait(&fw_cond, &fw_lock, &ts);
		}
		pthread_mutex_unlock(&fw_lock);
	}
	return NULL;
}

static int
fw_event(int fd, uint32_t mask, void *data)
{
	char buf[16];

	(void)mask;
	(void)data;
	while (read(fd, buf, sizeof(buf)) > 0)
		;
	pthread_mutex_lock(&fw_lock);
	fan_pub = fw_pub;
	pthread_mutex_unlock(&fw_lock);
	return 0;
}

/* Ask for the fast sample cadence — the fan popup calls this on every
 * render, so the cadence lapses back to idle once it closes. */
void
fanwatch_poke_fast(void)
{
	pthread_mutex_lock(&fw_lock);
	fw_fast_until_ms = monotonic_msec() + FW_FAST_HOLD_MS;
	pthread_cond_signal(&fw_cond);
	pthread_mutex_unlock(&fw_lock);
}

static void
fw_queue(FwCtl c)
{
	pthread_mutex_lock(&fw_lock);
	if (fw_run && fw_npending < FW_MAX_PENDING)
		fw_pending[fw_npending++] = c;
	pthread_cond_signal(&fw_cond);
	pthread_mutex_unlock(&fw_lock);
}

void
fanwatch_set_frac(int flat, double frac)
{
	FwCtl c = { FW_CTL_FRAC, flat, frac, 0, {{0},{0},0} };

	fw_queue(c);
}

void
fanwatch_set_auto(int flat)
{
	FwCtl c = { FW_CTL_AUTO, flat, 0.0, 0, {{0},{0},0} };

	fw_queue(c);
}

void
fanwatch_set_boost(int on)
{
	FwCtl c = { FW_CTL_BOOST, 0, 0.0, on, {{0},{0},0} };

	fw_queue(c);
}

void
fanwatch_set_curve(int flat, const FanCurve *curve)
{
	FwCtl c = { FW_CTL_CURVE, flat, 0.0, 0, *curve };

	fw_queue(c);
}

void
fanwatch_init(void)
{
	if (pipe2(fw_pipe, O_CLOEXEC | O_NONBLOCK) < 0)
		return;
	fw_src = wl_event_loop_add_fd(event_loop, fw_pipe[0],
			WL_EVENT_READABLE, fw_event, NULL);
	fw_run = 1;
	if (pthread_create(&fw_thread, NULL, fw_worker, NULL) != 0) {
		fw_run = 0;
		wlr_log(WLR_ERROR, "fanwatch: thread start failed — "
			"fan module unavailable");
	}
}
