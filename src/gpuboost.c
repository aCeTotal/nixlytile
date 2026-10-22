#include "nixlytile.h"

#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Idle iGPU clocks miss vblanks. */
#define GPUBOOST_HOLD_NS 300000000ULL

typedef struct {
	const char *driver;
	const char *min, *rpn, *rp0;
} FreqKnobs;

static const FreqKnobs knob_table[] = {
	{ "i915", "gt_min_freq_mhz", "gt_RPn_freq_mhz", "gt_RP0_freq_mhz" },
	{ "xe", "device/tile0/gt0/freq0/min_freq",
		"device/tile0/gt0/freq0/rpn_freq",
		"device/tile0/gt0/freq0/rp0_freq" },
};

static char min_path[PATH_MAX];
static char marker_path[PATH_MAX];
static int base_mhz, boost_mhz;
static int boosted;
static uint64_t last_kick_ns;
static struct wl_event_source *release_timer;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static int want, have;

static int
read_mhz(const char *path)
{
	FILE *f = fopen(path, "r");
	int v = -1;

	if (!f)
		return -1;
	if (fscanf(f, "%d", &v) != 1)
		v = -1;
	fclose(f);
	return v;
}

static void
write_mhz(const char *path, int mhz)
{
	char buf[16];
	int n = snprintf(buf, sizeof(buf), "%d\n", mhz);
	int fd = open(path, O_WRONLY | O_CLOEXEC);

	if (fd < 0)
		return;
	if (write(fd, buf, (size_t)n) != n)
		wlr_log(WLR_DEBUG, "gpuboost: %s <- %d failed", path, mhz);
	close(fd);
}

/* Survives a compositor crash. */
static void
mark_boosted(int on)
{
	FILE *f;

	if (!on) {
		unlink(marker_path);
		return;
	}
	if ((f = fopen(marker_path, "w"))) {
		fprintf(f, "%d\n", base_mhz);
		fclose(f);
	}
}

/* sysfs writes can block. */
static void *
boost_worker(void *data)
{
	int on;

	(void)data;
	pthread_setname_np(pthread_self(), "nixly-gpuboost");
	pthread_mutex_lock(&lock);
	for (;;) {
		while (want == have)
			pthread_cond_wait(&cond, &lock);
		on = want;
		pthread_mutex_unlock(&lock);
		if (on)
			mark_boosted(1);
		write_mhz(min_path, on ? boost_mhz : base_mhz);
		if (!on)
			mark_boosted(0);
		pthread_mutex_lock(&lock);
		have = on;
	}
	return NULL;
}

static void
request(int on)
{
	boosted = on;
	pthread_mutex_lock(&lock);
	want = on;
	pthread_cond_signal(&cond);
	pthread_mutex_unlock(&lock);
}

static int
release_cb(void *data)
{
	uint64_t idle = get_time_ns() - last_kick_ns;

	(void)data;
	if (idle < GPUBOOST_HOLD_NS) {
		wl_event_source_timer_update(release_timer,
			(int)((GPUBOOST_HOLD_NS - idle) / 1000000ULL) + 1);
		return 0;
	}
	request(0);
	return 0;
}

static const FreqKnobs *
render_gpu_knobs(char *dir, size_t len)
{
	const GpuInfo *g;
	size_t i;

	if (nvidia_render_primary || integrated_gpu_idx < 0)
		return NULL;
	g = &detected_gpus[integrated_gpu_idx];
	for (i = 0; i < LENGTH(knob_table); i++) {
		if (strcmp(g->driver, knob_table[i].driver) != 0)
			continue;
		snprintf(dir, len, "/sys/class/drm/card%d", g->card_index);
		return &knob_table[i];
	}
	return NULL;
}

void
gpuboost_init(void)
{
	char dir[64], path[PATH_MAX];
	const FreqKnobs *k = render_gpu_knobs(dir, sizeof(dir));
	const char *rt = getenv("XDG_RUNTIME_DIR");
	pthread_t worker;
	int rpn, rp0, crashed;

	if (!k || !rt)
		return;
	snprintf(min_path, sizeof(min_path), "%s/%s", dir, k->min);
	snprintf(marker_path, sizeof(marker_path), "%s/nixlytile-gpuboost", rt);
	if (access(min_path, W_OK) != 0) {
		wlr_log(WLR_INFO, "gpuboost: %s not writable", min_path);
		return;
	}
	snprintf(path, sizeof(path), "%s/%s", dir, k->rpn);
	rpn = read_mhz(path);
	snprintf(path, sizeof(path), "%s/%s", dir, k->rp0);
	rp0 = read_mhz(path);
	crashed = read_mhz(marker_path);
	base_mhz = crashed > 0 ? crashed : read_mhz(min_path);
	if (crashed > 0)
		write_mhz(min_path, base_mhz);
	unlink(marker_path);
	if (rpn <= 0 || rp0 <= rpn || base_mhz <= 0)
		return;
	boost_mhz = rpn + (rp0 - rpn) * 2 / 3;
	/* Clocks already pinned high. */
	if (base_mhz >= boost_mhz)
		return;
	if (pthread_create(&worker, NULL, boost_worker, NULL) != 0)
		return;
	pthread_detach(worker);
	release_timer = wl_event_loop_add_timer(event_loop, release_cb, NULL);
	wlr_log(WLR_INFO, "gpuboost: %s min %d -> %d MHz while animating",
		k->driver, base_mhz, boost_mhz);
}

void
gpuboost_kick(uint64_t now_ns)
{
	if (!release_timer)
		return;
	last_kick_ns = now_ns;
	if (boosted)
		return;
	request(1);
	wl_event_source_timer_update(release_timer,
		(int)(GPUBOOST_HOLD_NS / 1000000ULL));
}

void
gpuboost_finish(void)
{
	if (!release_timer)
		return;
	write_mhz(min_path, base_mhz);
	unlink(marker_path);
}
