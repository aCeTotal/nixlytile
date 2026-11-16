#include "nixlytile.h"

#include <pthread.h>
#include <sys/resource.h>
#include <time.h>

#include "camera.h"
#include "cpuload.h"
#include "mailbox.h"
#include "tracker.h"
#include "worker.h"

#define IDLE_INTERVAL_S 0.1
#define FLOOR_FPS 30
#define SLACK_S 0.005
#define LOAD_PERIOD_S 1.0
#define RECOVER_S 3.0
#define COST_RATE 0.1
#define STRAIN 0.8         /* share of the frame budget */
#define CALM 0.6
#define BUSY_LIMIT 0.85f   /* share of all CPUs */
#define CALM_BUSY 0.75f
#define FRAME_WAIT_MS 200
#define RETRY_MS 2000
#define LOST_HOLD_S 0.3
#define GAZE_NICE 10

/* Thread-owned streaming state. */
typedef struct {
	Camera cam;
	Tracker tracker;
	GazeModel model;
	int has_model;
	unsigned gen;
	Smooth sx;
	double last_ok;
	double last_frame;      /* last processed capture */
	double cost;            /* seconds per frame, smoothed */
	CpuLoad cpu;
	double load_t, calm_since;
	int throttled;          /* FLOOR_FPS under load */
	int failing;            /* last open failed */
	int tw, th;             /* tracker frame size */
} Stream;

static pthread_t thread;

static void
refresh_model(Stream *s)
{
	unsigned gen = mailbox_model_gen();

	if (gen == s->gen)
		return;
	s->has_model = mailbox_copy_model(&s->model);
	s->gen = gen;
}

static void
publish(Stream *s, const Features *f, double t)
{
	if (!s->has_model)
		return;
	s->last_ok = t;
	mailbox_publish(smooth(&s->sx, model_predict(&s->model, f), t));
}

/* Brief dropouts keep the estimate. */
static void
lose(const Stream *s, double t)
{
	if (t - s->last_ok > LOST_HOLD_S)
		mailbox_clear();
}

static double
seconds(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int
strained(const Stream *s, float busy)
{
	return s->cost > STRAIN / s->cam.fps || busy > BUSY_LIMIT;
}

static int
calm(const Stream *s, float busy)
{
	return s->cost < CALM / s->cam.fps && busy < CALM_BUSY;
}

/* FLOOR_FPS while CPU is strained. */
static void
pace(Stream *s, double t)
{
	float busy;
	int was = s->throttled;

	if (s->cam.fps <= FLOOR_FPS || t - s->load_t < LOAD_PERIOD_S)
		return;
	s->load_t = t;
	busy = cpuload_sample(&s->cpu);
	if (strained(s, busy) || !calm(s, busy))
		s->calm_since = t;
	s->throttled = strained(s, busy) || (s->throttled && t - s->calm_since < RECOVER_S);
	if (s->throttled != was)
		nx_log(NX_INFO, "gaze: tracking at %d fps", s->throttled ? FLOOR_FPS : s->cam.fps);
}

/* Seconds between processed frames. */
static double
spacing(const Stream *s)
{
	if (!mailbox_active())
		return IDLE_INTERVAL_S;
	return s->throttled ? 1.0 / FLOOR_FPS : 0;
}

/* 0 once camera is gone. */
static int
step(Stream *s)
{
	Frame f;
	Features ft;
	double t, began;
	Track track;
	CameraStatus st = camera_next(&s->cam, &f, FRAME_WAIT_MS);

	if (st != CAMERA_FRAME || f.t - s->last_frame < spacing(s) - SLACK_S)
		return st != CAMERA_GONE;
	t = s->last_frame = f.t;
	began = seconds();
	track = tracker_step(&s->tracker, &f, &ft);
	s->cost += COST_RATE * (seconds() - began - s->cost);
	pace(s, t);
	if (mailbox_calibrating())
		mailbox_hand_over(&ft, track == TRACK_OK);
	refresh_model(s);
	if (track == TRACK_OK)
		publish(s, &ft, t);
	if (track == TRACK_LOST)
		lose(s, t);
	return 1;
}

/* Models stay loaded between streams. */
static int
ensure_tracker(Stream *s)
{
	if (s->tracker.mesh && s->tw == s->cam.w && s->th == s->cam.h)
		return 0;
	if (s->tracker.mesh)
		tracker_close(&s->tracker);
	s->tw = s->cam.w;
	s->th = s->cam.h;
	return tracker_open(&s->tracker, s->cam.w, s->cam.h);
}

static void
stream(Stream *s)
{
	int alive = 1;

	if (camera_open(&s->cam) < 0 || ensure_tracker(s) < 0) {
		if (!s->failing)
			nx_log(NX_ERROR, "gaze: no usable camera or models");
		s->failing = 1;
		camera_close(&s->cam);
		mailbox_pause(RETRY_MS);
		return;
	}
	s->failing = 0;
	nx_log(NX_INFO, "gaze: tracking with %s %dx%d at %d fps", s->cam.path, s->cam.w, s->cam.h,
		s->cam.fps);
	while (alive && mailbox_wanted())
		alive = step(s);
	camera_close(&s->cam);
	mailbox_clear();
	nx_log(NX_INFO, "gaze: camera %s", alive ? "off" : "lost");
}

static void *
gaze_main(void *data)
{
	Stream s = { .gen = (unsigned)-1 };

	(void)data;
	pthread_setname_np(pthread_self(), "nixly-gaze");
	pthread_setschedparam(pthread_self(), SCHED_BATCH, &(struct sched_param){ 0 });
	setpriority(PRIO_PROCESS, (id_t)gettid(), GAZE_NICE);
	while (mailbox_await())
		stream(&s);
	tracker_close(&s.tracker);
	return NULL;
}

int
worker_start(void)
{
	if (mailbox_open() < 0)
		return -1;
	if (pthread_create(&thread, NULL, gaze_main, NULL) == 0)
		return 0;
	mailbox_stop();
	mailbox_close();
	return -1;
}

void
worker_stop(void)
{
	if (!mailbox_stop())
		return;
	pthread_join(thread, NULL);
	mailbox_close();
}
