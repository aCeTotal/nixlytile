/*
 * lightsense.c — webcam-based auto brightness for the light module.
 *
 * Two modes.  Auto (default at every login): camwatch.c's worker thread
 * grabs one low-res frame from the first working /dev/video* every
 * LS_INTERVAL_MS, the mean luma maps to a backlight target (dark room →
 * low, normal room → ~40%, bright room → high) and is applied with
 * hysteresis.  Manual: any hand adjustment (scroll, slider drag, popup
 * button) locks the level for the rest of the session.  The manual
 * value is saved in ~/.local/nixlyos/ and remembered across logins, but
 * the mode itself always starts as Auto.
 *
 * All V4L2 work lives on camwatch's thread — a dequeue can block for
 * hundreds of ms, and the fork this used to need to escape the event
 * loop stalled the cursor by itself.
 */
#include "nixlytile.h"

#include <pwd.h>

#define LS_INTERVAL_MS 30000
#define LS_FIRST_MS    2500
#define LS_HYSTERESIS  5.0
#define LS_AC_PERCENT  60.0

int light_auto_mode = 1;
double light_manual_value = -1.0;
int light_ambient_luma = -1;

/* Wall power: hold LS_AC_PERCENT and skip the ambient loop entirely —
 * auto brightness is a battery-saving measure.  Manual adjustments
 * still work (they switch the mode to manual as usual). */
static int ls_on_ac;

static struct wl_event_source *ls_timer;
static char ls_conf_path[PATH_MAX];

static void
ls_resolve_path(void)
{
	const char *home = getenv("HOME");

	if (!home) {
		struct passwd *pw = getpwuid(getuid());
		if (pw)
			home = pw->pw_dir;
	}
	if (!home)
		home = "/";
	snprintf(ls_conf_path, sizeof(ls_conf_path),
			"%s/.local/nixlyos/brightness.conf", home);
}

static void
ls_save(void)
{
	FILE *fp;
	char dir[PATH_MAX];
	char *slash;

	if (!ls_conf_path[0])
		return;
	snprintf(dir, sizeof(dir), "%s", ls_conf_path);
	slash = strrchr(dir, '/');
	if (slash) {
		*slash = '\0';
		mkdir(dir, 0755);
	}
	fp = fopen(ls_conf_path, "w");
	if (!fp)
		return;
	fprintf(fp, "manual %.1f\n", light_manual_value);
	fclose(fp);
}

static void
ls_load(void)
{
	FILE *fp = fopen(ls_conf_path, "r");
	double v;

	if (!fp)
		return;
	if (fscanf(fp, "manual %lf", &v) == 1 && v >= 0.0 && v <= 100.0)
		light_manual_value = v;
	fclose(fp);
}

/* Mean luma → backlight percent: pitch dark 13, dim ≈13-20, normal
 * indoor ≈20-30, above that climbs fast — direct daylight hits 100. */
static double
ls_target(int luma)
{
	if (luma <= 25)
		return 13.0;
	if (luma <= 85)
		return 13.0 + (luma - 25) * (20.0 - 13.0) / 60.0;
	if (luma <= 150)
		return 20.0 + (luma - 85) * (30.0 - 20.0) / 65.0;
	if (luma >= 210)
		return 100.0;
	return 30.0 + (luma - 150) * (100.0 - 30.0) / 60.0;
}

static void
ls_apply(int luma)
{
	double target, cur;

	light_ambient_luma = luma;
	if (!light_auto_mode || ls_on_ac || !backlight_available)
		return;
	target = ls_target(luma);
	cur = backlight_percent();
	if (cur >= 0.0 && fabs(target - cur) < LS_HYSTERESIS)
		return;
	if (set_backlight_percent(target) != 0)
		return;
	light_last_percent = target;
	light_cached_percent = target;
	refreshstatuslight();
}

/* camwatch.c hands every published frame here, whoever asked for it —
 * on laptops presence.c drives the cadence and this just consumes. */
void
lightsense_camera_frame(const CamSnapshot *s)
{
	ls_apply(s->mean);
}

static int
ls_sample(void *data)
{
	(void)data;
	if (ls_timer)
		wl_event_source_timer_update(ls_timer, LS_INTERVAL_MS);
	/* presence.c owns the camera cadence on laptops — asking here too
	 * would light the webcam LED during normal use. */
	if (!light_auto_mode || ls_on_ac || presence_active())
		return 0;
	camwatch_request();
	return 0;
}

void
lightsense_sample_now(void)
{
	/* presence.c owns the camera on laptops — route through it so the
	 * grab happens even during input-activity (single LED blip) */
	if (presence_active()) {
		presence_sample_once();
		return;
	}
	if (ls_timer)
		wl_event_source_timer_update(ls_timer, 1);
}

/* Hand adjustment: lock the level for this session and remember it. */
void
light_mode_set_manual(double value)
{
	light_auto_mode = 0;
	if (value >= 0.0 && value <= 100.0)
		light_manual_value = value;
	ls_save();
}

void
light_mode_set_auto(void)
{
	light_auto_mode = 1;
	lightsense_sample_now();
}

/* powersave.c reports AC/battery transitions.  Plugging in pins the
 * backlight at LS_AC_PERCENT; unplugging hands control back to the
 * ambient loop (if the mode is still Auto). */
void
lightsense_power_event(int on_ac)
{
	if (on_ac == ls_on_ac)
		return;
	ls_on_ac = on_ac;
	if (on_ac) {
		if (!backlight_available)
			return;
		if (set_backlight_percent(LS_AC_PERCENT) != 0)
			return;
		light_last_percent = LS_AC_PERCENT;
		light_cached_percent = LS_AC_PERCENT;
		refreshstatuslight();
	} else if (light_auto_mode) {
		lightsense_sample_now();
	}
}

void
lightsense_init(void)
{
	ls_resolve_path();
	ls_load();
	if (!ls_timer)
		ls_timer = wl_event_loop_add_timer(event_loop, ls_sample, NULL);
	if (ls_timer)
		wl_event_source_timer_update(ls_timer, LS_FIRST_MS);
}
