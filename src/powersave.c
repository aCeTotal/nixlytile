/*
 * powersave.c — battery saving on discharge, full performance on power.
 *
 * The rule: wall power (or a desktop with no battery at all) = the
 * machine ALWAYS runs at its absolute best — performance profile, turbo
 * on, clocks uncapped.  Battery = max saving — low-power profile, turbo
 * off, clocks capped hard, compositor FPS capped at 60 if the user's
 * limiter is off.  A 5s poll of the battery status flips between the
 * two on transitions (and once at startup), so manual profile/limiter
 * changes made in between are left alone.  Desktops assert performance
 * once at startup and need no poll.
 */
#include "nixlytile.h"

#define PS_POLL_MS 5000

static struct wl_event_source *ps_timer;
static int ps_engaged = -1;     /* -1 unknown → first tick always applies */
static int ps_fps_limited;      /* we enabled the limiter, not the user */

static int
ps_on_battery(void)
{
	static const char *paths[] = {
		"/sys/class/power_supply/BAT0/status",
		"/sys/class/power_supply/BAT1/status",
	};

	for (size_t i = 0; i < LENGTH(paths); i++) {
		char buf[32];
		FILE *fp = fopen(paths[i], "r");

		if (!fp)
			continue;
		if (fgets(buf, sizeof(buf), fp)) {
			fclose(fp);
			return strncmp(buf, "Discharging", 11) == 0;
		}
		fclose(fp);
	}
	return 0;
}

/* Re-apply whatever clock regime the power state calls for.  Called
 * internally and by presence.c when leaving its (deeper) save mode, so
 * waking on battery lands back on the battery cap, not full clocks. */
void
powersave_reassert(void)
{
	if (ps_engaged == 1) {
		power_profile_low();
		cpuclock_boost(0);
		cpuclock_cap(0.4);
	} else {
		power_profile_high();
		cpuclock_boost(1);
		cpuclock_restore();
	}
}

static void
ps_engage(void)
{
	powersave_reassert();

	if (!fps_limit_enabled) {
		fps_limit_enabled = 1;
		if (fps_limit_value > 60)
			fps_limit_value = 60;
		ps_fps_limited = 1;
	}
	wlr_log(WLR_INFO, "powersave: on battery — max saving engaged");
}

/* Wall power: absolute best, always. */
static void
ps_full_performance(void)
{
	powersave_reassert();

	if (ps_fps_limited) {
		fps_limit_enabled = 0;
		ps_fps_limited = 0;
	}
	wlr_log(WLR_INFO, "powersave: on wall power — full performance");
}

static int
ps_tick(void *data)
{
	int on_bat = ps_on_battery();

	(void)data;
	if (ps_timer)
		wl_event_source_timer_update(ps_timer, PS_POLL_MS);
	if (on_bat && ps_engaged != 1) {
		ps_engaged = 1;
		ps_engage();
	} else if (!on_bat && ps_engaged != 0) {
		ps_engaged = 0;
		ps_full_performance();
	}
	return 0;
}

void
powersave_init(void)
{
	struct stat st;

	if (stat("/sys/class/power_supply/BAT0", &st) != 0 &&
			stat("/sys/class/power_supply/BAT1", &st) != 0) {
		/* Desktop on fixed power: assert absolute best once. */
		ps_engaged = 0;
		ps_full_performance();
		return;
	}
	if (!ps_timer)
		ps_timer = wl_event_loop_add_timer(event_loop, ps_tick, NULL);
	if (ps_timer)
		wl_event_source_timer_update(ps_timer, PS_POLL_MS);
	ps_tick(NULL);   /* apply the current state right away */
}
