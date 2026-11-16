#include <stdio.h>

#include "calib.h"
#include "mailbox.h"

#define FIND_TIMEOUT_MS 8000
#define TRAVEL_MS 450
#define SETTLE_MS 450
#define COLLECT_MS 1000
#define POINT_MS (TRAVEL_MS + SETTLE_MS + COLLECT_MS)
#define TICK_MS 16
#define MAX_SAMPLES 640
#define MAX_ERROR 0.4f

typedef enum { CAL_OFF, CAL_FIND, CAL_RUN } Phase;

typedef struct {
	float x, y;
} Spot;

/* Columns repeat, so hold-outs interpolate. */
static const Spot targets[] = {
	{ 0.5f, 0.5f }, { 0.06f, 0.3f }, { 0.28f, 0.3f }, { 0.72f, 0.3f }, { 0.94f, 0.3f },
	{ 0.94f, 0.7f }, { 0.72f, 0.7f }, { 0.28f, 0.7f }, { 0.06f, 0.7f },
};

static struct {
	Phase phase;
	Monitor *m;
	int point;
	uint64_t since;
	Sample samples[MAX_SAMPLES];
	int n;
	struct wl_event_source *tick;
	CalibUi ui;
} cal;

static int
alive(const Monitor *target)
{
	Monitor *m;

	wl_list_for_each(m, &mons, link)
		if (m == target)
			return 1;
	return 0;
}

static float
ease(float t)
{
	t = t < 0 ? 0 : t > 1 ? 1 : t;
	return t * t * (3 - 2 * t);
}

static Marker
marker_at(uint64_t t)
{
	Spot from = targets[cal.point ? cal.point - 1 : 0], to = targets[cal.point];
	float k = ease((float)t / TRAVEL_MS);
	float ring = 1.0f - ((float)t - TRAVEL_MS) / (SETTLE_MS + COLLECT_MS);

	return (Marker){ from.x + (to.x - from.x) * k, from.y + (to.y - from.y) * k,
		ring < 0 ? 0 : ring > 1 ? 1 : ring };
}

static void
conclude(void)
{
	Profile p = {0};
	char msg[128];

	snprintf(p.output, sizeof(p.output), "%s", cal.m->nx_output->name);
	if (model_fit(&p.model, cal.samples, cal.n) < 0 || p.model.error > MAX_ERROR) {
		nx_log(NX_INFO, "gaze: calibration rejected, %d samples, error %.3f", cal.n,
			(double)p.model.error);
		calib_stop("Kalibreringen ble for unøyaktig – prøv igjen");
		return;
	}
	gaze_calibrated(&p, cal.samples, cal.n);
	nx_log(NX_INFO, "gaze: calibrated, error %.3f of width", (double)p.model.error);
	snprintf(msg, sizeof(msg), "Blikksporing kalibrert · ±%.0f %% av skjermbredden",
		(double)p.model.error * 100);
	calib_stop(msg);
}

/* Next target; 0 after last. */
static int
advance(uint64_t t)
{
	if (cal.phase != CAL_RUN || t < POINT_MS)
		return 1;
	cal.since += POINT_MS;
	return ++cal.point < (int)LENGTH(targets);
}

static int
tick(void *data)
{
	uint64_t t = monotonic_msec() - cal.since;
	Marker mk;

	(void)data;
	if (!alive(cal.m)) {
		calib_stop(NULL);
		return 0;
	}
	if (cal.phase == CAL_FIND && t > FIND_TIMEOUT_MS) {
		calib_stop("Fant ikke ansiktet ditt – sitt foran kameraet");
		return 0;
	}
	if (!advance(t)) {
		conclude();
		return 0;
	}
	mk = marker_at(monotonic_msec() - cal.since);
	if (cal.phase == CAL_RUN)
		calib_ui_marker(&cal.ui, cal.m, &mk);
	wl_event_source_timer_update(cal.tick, TICK_MS);
	return 0;
}

void
calib_start(Monitor *m)
{
	if (cal.phase != CAL_OFF || !m || !m->nx_output)
		return;
	cal.phase = CAL_FIND;
	cal.m = m;
	cal.point = 0;
	cal.n = 0;
	cal.since = monotonic_msec();
	calib_ui_show(&cal.ui, m, "Følg prikken med blikket");
	cal.tick = wl_event_loop_add_timer(event_loop, tick, NULL);
	wl_event_source_timer_update(cal.tick, TICK_MS);
	mailbox_calibrate(1);
}

void
calib_stop(const char *msg)
{
	if (cal.phase == CAL_OFF)
		return;
	mailbox_calibrate(0);
	wl_event_source_remove(cal.tick);
	calib_ui_hide(&cal.ui);
	if (msg && alive(cal.m))
		osd_show_force(cal.m, msg);
	cal.phase = CAL_OFF;
	cal.tick = NULL;
	cal.m = NULL;
}

int
calib_active(void)
{
	return cal.phase != CAL_OFF;
}

/* Samples once eyes settle. */
void
calib_feed(void)
{
	Features f;
	int ok;

	mailbox_drain();
	ok = mailbox_features(&f);
	if (cal.phase == CAL_FIND && ok) {
		cal.phase = CAL_RUN;
		cal.since = monotonic_msec();
	}
	if (cal.phase != CAL_RUN || !ok || cal.n == MAX_SAMPLES ||
	    monotonic_msec() - cal.since < TRAVEL_MS + SETTLE_MS ||
	    monotonic_msec() - cal.since >= POINT_MS)
		return;
	cal.samples[cal.n++] = (Sample){ f, targets[cal.point].x, cal.point };
}
