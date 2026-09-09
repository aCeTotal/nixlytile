/*
 * powersave.c — battery saving on discharge, full performance on power.
 *
 * The rule: wall power (or a desktop with no battery at all) = the
 * machine ALWAYS runs at its absolute best — performance profile, turbo
 * on, clocks uncapped.  Battery = max saving — low-power profile, turbo
 * off, clocks capped hard, compositor FPS capped at 60 if the user's
 * limiter is off.  Applied only on transitions (and once at startup),
 * so manual profile/limiter changes in between are left alone.
 * Battery state comes from
 * battwatch.c's worker thread — this file never touches sysfs, so the
 * EC's ~100ms status read can't stall the compositor thread.
 */
#include "nixlytile.h"

/* Battery clock ceiling, as a fraction of each policy's min..max range.
 * Shared with powersave_clock_cap() so launchboost.c restores exactly what
 * powersave_reassert() set. */
#define PS_BATTERY_CAP 0.6

static int ps_engaged = -1;     /* -1 unknown → first event always applies */
static int ps_fps_limited;      /* we enabled the limiter, not the user */

/* Re-apply whatever clock regime the power state calls for.  Called
 * internally and by presence.c when leaving its (deeper) save mode, so
 * waking on battery lands back on the battery cap, not full clocks. */
void
powersave_reassert(void)
{
	if (ps_engaged == 1) {
		power_profile_low();
		cpuclock_perf(0);       /* powersave governor + EPP power */
		cpuclock_boost(0);
		cpuclock_cap(PS_BATTERY_CAP);  /* mild cap: powersave governor + EPP
					 * already idle the clocks down; 0.35
					 * (1.29 GHz ceiling) made everything
					 * crawl while saving almost nothing —
					 * idle draw is panel/platform-bound */
		output_lowpower_refresh(1);  /* panels → lowest refresh (60 Hz) */
		unfocused_fps_cap = 20; /* visible background tiles stay watchable —
					 * 2 froze a side-by-side video outright,
					 * which reads as a broken compositor rather
					 * than as power saving */
	} else {
		power_profile_high();
		cpuclock_perf(1);       /* performance governor + EPP perf */
		cpuclock_boost(1);
		cpuclock_restore();
		output_lowpower_refresh(0);  /* restore pinned/best refresh */
		unfocused_fps_cap = 0;  /* no throttle on wall power — "always at
					 * its absolute best" applies to every
					 * visible tile, not just the focused one */
	}
}

/* The clock cap the current power state wants.  launchboost.c releases it
 * for the length of a launch and puts this value back, so the two never
 * disagree about what "normal" is.  Read from the boost worker thread —
 * ps_engaged is a plain int written only here, so a stale read costs at
 * most one boost window at the wrong cap. */
double
powersave_clock_cap(void)
{
	return ps_engaged == 1 ? PS_BATTERY_CAP : 1.0;
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

/* Called from battwatch's event-loop callback whenever the published
 * battery snapshot changes (and once at startup). */
void
powersave_battery_event(void)
{
	BattSnapshot s;
	int on_bat;

	if (!battwatch_get(&s))
		return;
	on_bat = s.available &&
		strncmp(s.status, "Discharging", 11) == 0;
	if (on_bat && ps_engaged != 1) {
		ps_engaged = 1;
		ps_engage();
		lightsense_power_event(0);
	} else if (!on_bat && ps_engaged != 0) {
		ps_engaged = 0;
		ps_full_performance();
		lightsense_power_event(1);
	}
}

void
powersave_init(void)
{
	/* State arrives from battwatch; nothing to poll here.  Until the
	 * first snapshot lands the machine keeps its boot defaults. */
}
