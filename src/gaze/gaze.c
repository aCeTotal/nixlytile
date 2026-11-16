#include <math.h>
#include <string.h>

#include "calib.h"
#include "vrslayer/link.h"
#include "mailbox.h"
#include "worker.h"

#define GAZE_KEY "gaze-tracking"
#define DEAD_ZONE 0.04f

static struct {
	int enabled;
	int calibrated;
	Profile profile;
	float shown;            /* focus last published */
	struct wl_event_source *src;
} gz = { .enabled = 1, .shown = 0.5f };

static int
on_features(int fd, uint32_t mask, void *data)
{
	(void)fd;
	(void)mask;
	(void)data;
	calib_feed();
	return 0;
}

void
gaze_setup(void)
{
	gz.calibrated = profile_load(&gz.profile) == 0;
	mailbox_model(gz.calibrated ? &gz.profile.model : NULL);
	if (worker_start() < 0)
		return;
	gz.src = wl_event_loop_add_fd(event_loop, mailbox_fd(), WL_EVENT_READABLE, on_features, NULL);
}

void
gaze_cleanup(void)
{
	calib_stop(NULL);
	if (gz.src)
		wl_event_source_remove(gz.src);
	gz.src = NULL;
	worker_stop();
}

void
gaze_conf_reset(void)
{
	gz.enabled = 1;
}

int
gaze_conf_parse(const char *key, const char *value)
{
	if (strcmp(key, GAZE_KEY))
		return 0;
	gz.enabled = strcmp(value, "0") != 0;
	return 1;
}

void
gaze_calibrated(const Profile *p, const Sample *s, int n)
{
	gz.profile = *p;
	gz.calibrated = 1;
	mailbox_model(&p->model);
	if (profile_save(p, s, n) < 0)
		nx_log(NX_ERROR, "gaze: profile not saved");
}

/* Horizontal focus; jitter never reuploads. */
uint32_t
gaze_focus(Monitor *m, Client *game)
{
	GazeSample s;
	float x;

	if (!gz.enabled || !gz.calibrated || !game || !m->nx_output || game->geom.width <= 0 ||
	    strcmp(m->nx_output->name, gz.profile.output))
		return VRS_FOCUS_NONE;
	mailbox_demand();
	s = mailbox_sample();
	x = ((float)m->m.x + s.x * (float)m->m.width - (float)game->geom.x) / (float)game->geom.width;
	if (!s.valid || x < 0 || x > 1)
		return VRS_FOCUS_NONE;
	if (fabsf(x - gz.shown) > DEAD_ZONE)
		gz.shown = x;
	return (uint32_t)(gz.shown * VRS_UNIT) << VRS_FOCUS_SHIFT | VRS_UNIT / 2;
}

void
gazecalibrate(const Arg *arg)
{
	(void)arg;
	if (calib_active()) {
		calib_stop("Kalibrering avbrutt");
		return;
	}
	if (!gz.enabled) {
		osd_show_force(selmon, "Blikksporing er av (gaze-tracking=0)");
		return;
	}
	calib_start(selmon);
}
