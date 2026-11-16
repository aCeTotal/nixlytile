/* Battery saving, full speed on wall power. */
#include "nixlytile.h"

/* Battery clock ceiling, as a fraction of each policy's min..max range.
 * Shared with powersave_clock_cap() so launchboost.c restores exactly what
 * powersave_reassert() set. */
#define PS_BATTERY_CAP 0.6
#define PS_AC_BRIGHTNESS 60.0

static int ps_engaged = -1;     /* -1 unknown → first event always applies */

/* Re-applies the power state's regime. */
void
powersave_reassert(void)
{
	/* The sysfs regime (profile + governor + EPP + cap — ~100+ writes,
	 * the EC profile alone ~100 ms) runs on cpuclock.c's worker; only
	 * the modeset and the fps-cap flag stay on the compositor thread. */
	if (ps_engaged == 1) {
		cpuclock_regime_battery_async(PS_BATTERY_CAP);
					/* mild cap: powersave governor + EPP
					 * already idle the clocks down; 0.35
					 * (1.29 GHz ceiling) made everything
					 * crawl while saving almost nothing —
					 * idle draw is panel/platform-bound */
		unfocused_fps_cap = 20; /* visible background tiles stay watchable —
					 * 2 froze a side-by-side video outright,
					 * which reads as a broken compositor rather
					 * than as power saving */
	} else {
		if (!game_mode_active)
			cpuclock_regime_ac_async();
		unfocused_fps_cap = 0;  /* no throttle on wall power */
	}
	monconf_apply_modes();
	updatemons(NULL, NULL);
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
	nx_log(NX_INFO, "powersave: on battery — max saving engaged");
}

/* Wall power: quiet, games boost. */
static void
ps_wall_power(void)
{
	powersave_reassert();
	nx_log(NX_INFO, "powersave: on wall power — quiet profile");
}

/* Any thread: sysfs regime only. */
void
powersave_battery_reassert_async(void)
{
	if (ps_engaged == 1)
		cpuclock_regime_battery_async(PS_BATTERY_CAP);
}

static void
pin_ac_brightness(void)
{
	if (!backlight_available || set_backlight_percent(PS_AC_BRIGHTNESS) != 0)
		return;
	light_last_percent = PS_AC_BRIGHTNESS;
	light_cached_percent = PS_AC_BRIGHTNESS;
	refreshstatuslight();
}

/* Battwatch snapshot changed. */
void
powersave_battery_event(void)
{
	BattSnapshot s;
	int on_bat;

	if (!battwatch_get(&s))
		return;
	on_bat = s.available && (s.source == POWER_UNKNOWN ?
		strncmp(s.status, "Discharging", 11) == 0 :
		s.source == POWER_BATTERY);
	if (on_bat && ps_engaged != 1) {
		ps_engaged = 1;
		ps_engage();
	} else if (!on_bat && ps_engaged != 0) {
		ps_engaged = 0;
		ps_wall_power();
		pin_ac_brightness();
	}
}

void
powersave_init(void)
{
	/* State arrives from battwatch; nothing to poll here.  Until the
	 * first snapshot lands the machine keeps its boot defaults. */
}
