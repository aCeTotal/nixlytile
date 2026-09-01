/*
 * presence.c — webcam presence watch → full power save.  Laptops only.
 *
 * Input activity alone proves presence, so while the keyboard/pointer
 * has been touched within PR_CAM_IDLE_MS the camera is never opened —
 * the webcam LED stays dark during normal use.  Only once input has
 * been idle that long does this file start asking camwatch.c's worker
 * thread for low-res frames (mean luma + an 8x6 grid of block means)
 * to tell "reading / watching" from "walked away": each frame is
 * affine-fitted onto the previous grid (a·p+b least squares, so
 * auto-exposure gain AND offset drift are never motion) and the
 * residual thresholded — a person in front of the screen produces
 * local block motion within a couple of samples.  All V4L2 work lives
 * on camwatch's thread, so neither the grab nor the fork it used to
 * need can stall the cursor.  No motion AND no input for PR_ABSENT_AFTER_MS →
 * save mode: nixly-lockscreen, backlight to 0, outputs off, lowest
 * power profile.  The first motion sample (polled every
 * PR_INTERVAL_ABSENT_MS) or any local input brings the outputs,
 * backlight and profile straight back — the lockscreen is already up,
 * so waking lands on it.
 *
 * camwatch hands every frame to lightsense.c too, so the two never
 * fight over the camera: while presence runs, lightsense's own sampler
 * stands down and routes explicit sample requests through
 * presence_sample_once().  Visible idle-inhibitors (video players)
 * block save-entry.
 */
#include "nixlytile.h"

#define PR_INTERVAL_PRESENT_MS 10000
#define PR_INTERVAL_ABSENT_MS  2500
#define PR_ABSENT_AFTER_MS     120000
/* Camera stays untouched (LED off) until input has been idle this long. */
#define PR_CAM_IDLE_MS         60000
/* Affine-compensated static-scene residual sits well below 1; a
 * person's micro-motion lands above 2, so 1.2 splits them with margin. */
#define PR_MOTION_THRESH       1.2
#define PR_DARK_DELAY_MS       1500   /* let the lockscreen map first */

uint64_t last_key_activity_ms;

static struct wl_event_source *pr_timer;
static struct wl_event_source *pr_dark_timer;
static int pr_laptop;
static unsigned char pr_prev[CAM_NBLOCKS];
static int pr_have_prev;
static uint64_t pr_last_motion_ms;
static int pr_saving;           /* 1 = lock spawned, 2 = dark applied */
static int pr_force_sample;     /* grab once even during input-active */
static double pr_saved_backlight = -1.0;

int
presence_active(void)
{
	return pr_laptop && pr_timer != NULL;
}

/* A visible idle-inhibitor (video player) means someone is watching. */
static int
pr_idle_inhibited(void)
{
	struct wlr_idle_inhibitor_v1 *inhibitor;
	int lx, ly;

	wl_list_for_each(inhibitor, &idle_inhibit_mgr->inhibitors, link) {
		struct wlr_surface *surface =
			wlr_surface_get_root_surface(inhibitor->surface);
		struct wlr_scene_tree *tree = surface->data;

		if (!tree || wlr_scene_node_coords(&tree->node, &lx, &ly))
			return 1;
	}
	return 0;
}

static void
pr_outputs_set(int enabled)
{
	Monitor *m;

	wl_list_for_each(m, &mons, link) {
		struct wlr_output_state st;

		if (!m->wlr_output)
			continue;
		wlr_output_state_init(&st);
		wlr_output_state_set_enabled(&st, enabled);
		wlr_output_commit_state(m->wlr_output, &st);
		wlr_output_state_finish(&st);
	}
}

static void
pr_spawn_lock(void)
{
	static const char *argv[] = { "nixly-lockscreen", NULL };

	if (locked)
		return;
	spawn_cmd_async(argv);
}

/* Stage 2: lockscreen has had time to map — go fully dark. */
static int
pr_go_dark(void *data)
{
	(void)data;
	if (pr_saving != 1)
		return 0;
	pr_saving = 2;

	pr_saved_backlight = backlight_percent();
	set_backlight_percent(0.0);

	/* nobody's watching: lowest profile, lowest clock, turbo off */
	power_profile_low();
	cpuclock_boost(0);
	cpuclock_cap(0.0);

	pr_outputs_set(0);
	wlr_log(WLR_INFO, "presence: nobody in front — full power save");
	return 0;
}

static void
pr_enter_save(void)
{
	pr_saving = 1;
	pr_spawn_lock();
	if (!pr_dark_timer)
		pr_dark_timer = wl_event_loop_add_timer(event_loop, pr_go_dark,
				NULL);
	if (pr_dark_timer)
		wl_event_source_timer_update(pr_dark_timer, PR_DARK_DELAY_MS);
}

static void
pr_exit_save(void)
{
	int was_dark = pr_saving == 2;

	pr_saving = 0;
	if (pr_dark_timer)
		wl_event_source_timer_update(pr_dark_timer, 0);
	if (!was_dark)
		return;

	pr_outputs_set(1);
	/* battery → back to the battery cap, wall power → absolute best */
	powersave_reassert();
	if (light_auto_mode) {
		lightsense_sample_now();
		/* interim level until the sample lands */
		if (pr_saved_backlight > 0.0)
			set_backlight_percent(pr_saved_backlight);
	} else if (pr_saved_backlight > 0.0) {
		set_backlight_percent(pr_saved_backlight);
	}
	wlr_log(WLR_INFO, "presence: someone's back — waking to lockscreen");
}

/* One explicit grab (lightsense wants a fresh ambient sample) even
 * while input-activity would normally keep the camera closed. */
void
presence_sample_once(void)
{
	if (!pr_laptop)
		return;
	pr_force_sample = 1;
	if (pr_timer)
		wl_event_source_timer_update(pr_timer, 1);
}

/* Called from keypress/motion: local input is presence, wake instantly. */
void
presence_note_input(void)
{
	if (!pr_laptop)
		return;
	pr_last_motion_ms = monotonic_msec();
	if (pr_saving)
		pr_exit_save();
}

/* camwatch.c hands every published frame here (compositor thread). */
void
presence_camera_frame(const CamSnapshot *s)
{
	if (!pr_laptop)
		return;

	if (pr_have_prev) {
		/* Affine-compensated block diff: least-squares fit
		 * grid ≈ a·prev + b, then threshold the residual.
		 * Auto-exposure changes are global gain/offset and fit out
		 * exactly; the old mean-subtraction only cancelled offset,
		 * so AE hunting in an empty room registered as motion and
		 * save mode never engaged. */
		double sp = 0, sg = 0, spp = 0, spg = 0;
		double a, b, den, diff = 0.0;

		for (int i = 0; i < CAM_NBLOCKS; i++) {
			sp += pr_prev[i];
			sg += s->grid[i];
			spp += (double)pr_prev[i] * pr_prev[i];
			spg += (double)pr_prev[i] * s->grid[i];
		}
		den = CAM_NBLOCKS * spp - sp * sp;
		a = den > 1e-6 ? (CAM_NBLOCKS * spg - sp * sg) / den : 1.0;
		/* runaway fit would mask real scene changes */
		if (a < 0.5)
			a = 0.5;
		if (a > 2.0)
			a = 2.0;
		b = (sg - a * sp) / CAM_NBLOCKS;
		for (int i = 0; i < CAM_NBLOCKS; i++)
			diff += fabs(s->grid[i] - (a * pr_prev[i] + b));
		diff /= CAM_NBLOCKS;
		if (diff > PR_MOTION_THRESH) {
			pr_last_motion_ms = monotonic_msec();
			if (pr_saving)
				pr_exit_save();
		}
	}
	memcpy(pr_prev, s->grid, sizeof(pr_prev));
	pr_have_prev = 1;
}

static int
pr_sample(void *data)
{
	uint64_t now = monotonic_msec();
	uint64_t idle_ref;

	(void)data;
	if (pr_timer)
		wl_event_source_timer_update(pr_timer, pr_saving ?
				PR_INTERVAL_ABSENT_MS : PR_INTERVAL_PRESENT_MS);

	/* Absence: no camera motion AND no local input for the window,
	 * nothing inhibiting idle, and not mid-game. */
	idle_ref = MAX(pr_last_motion_ms,
			MAX(last_pointer_motion_ms, last_key_activity_ms));
	if (!pr_saving && idle_ref &&
			now - idle_ref >= PR_ABSENT_AFTER_MS &&
			!pr_idle_inhibited() && !game_mode_active &&
			!fullscreen_video_playing())
		pr_enter_save();

	/* Recent input proves presence on its own: leave the camera (and
	 * its LED) alone until input has been idle a while.  The baseline
	 * grid is stale after a camera gap — drop it so the first grab of
	 * the next idle phase only re-baselines. */
	{
		uint64_t input_ms = MAX(last_pointer_motion_ms,
				last_key_activity_ms);

		if (!pr_saving && !pr_force_sample && input_ms &&
				now - input_ms < PR_CAM_IDLE_MS) {
			pr_have_prev = 0;
			return 0;
		}
	}
	pr_force_sample = 0;
	camwatch_request();
	return 0;
}

void
presence_init(void)
{
	struct stat st;

	/* Laptops only (backlight_available is probed lazily elsewhere, so
	 * gate on the battery — the definitive laptop signal). */
	pr_laptop = stat("/sys/class/power_supply/BAT0", &st) == 0 ||
		stat("/sys/class/power_supply/BAT1", &st) == 0;
	if (!pr_laptop)
		return;
	pr_last_motion_ms = monotonic_msec();
	/* single startup grab: gives lightsense its ambient baseline */
	pr_force_sample = 1;
	if (!pr_timer)
		pr_timer = wl_event_loop_add_timer(event_loop, pr_sample, NULL);
	if (pr_timer)
		wl_event_source_timer_update(pr_timer, PR_INTERVAL_PRESENT_MS);
}
