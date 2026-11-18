/* Battery saving, full speed on wall power. */
#include "nixlytile.h"

/* Battery clock ceiling, as a fraction of each policy's min..max range.
 * Shared with powersave_clock_cap() so launchboost.c restores exactly what
 * powersave_reassert() set. */
#define PS_BATTERY_CAP 0.6
#define PS_AC_BRIGHTNESS 60.0
#define PS_BATTERY_BRIGHTNESS 26.0
#define PS_SETTLE_MS 3000

static int ps_engaged = -1;     /* -1 unknown → first event always applies */
static int ps_settling;
static struct wl_event_source *ps_settle;

/* Re-applies the power state's regime. */
void
powersave_reassert(void)
{
	/* Sysfs writes run on cpuclock's worker. */
	if (ps_engaged == 1) {
		cpuclock_regime_battery_async(PS_BATTERY_CAP); /* 0.35 crawled, saved nothing */
		unfocused_fps_cap = 20; /* 2 froze background video */
		output_battery_refresh();
		return;
	}
	if (!game_mode_active)
		cpuclock_regime_ac_async();
	unfocused_fps_cap = 0;
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

int
powersave_on_battery(void)
{
	return ps_engaged == 1;
}

static void
set_brightness(double percent)
{
	if (!backlight_available || set_backlight_percent(percent) != 0)
		return;
	light_last_percent = percent;
	light_cached_percent = percent;
	refreshstatuslight();
}

static void
ps_engage(void)
{
	powersave_reassert();
	if (backlight_available && backlight_percent() > PS_BATTERY_BRIGHTNESS)
		set_brightness(PS_BATTERY_BRIGHTNESS);
	nx_log(NX_INFO, "powersave: on battery — max saving engaged");
}

/* Wall power: quiet, games boost. */
static void
ps_wall_power(void)
{
	powersave_reassert();
	set_brightness(PS_AC_BRIGHTNESS);
	nx_log(NX_INFO, "powersave: on wall power — quiet profile");
}

/* Any thread: sysfs regime only. */
void
powersave_battery_reassert_async(void)
{
	if (ps_engaged == 1)
		cpuclock_regime_battery_async(PS_BATTERY_CAP);
}

static int
on_battery_now(void)
{
	BattSnapshot s;

	if (!battwatch_get(&s) || !s.available)
		return 0;
	if (s.source == POWER_UNKNOWN)
		return strncmp(s.status, "Discharging", 11) == 0;
	return s.source == POWER_BATTERY;
}

static int
ps_settled(void *data)
{
	(void)data;
	ps_settling = 0;
	if (ps_engaged == 1 || !on_battery_now())
		return 0;
	ps_engaged = 1;
	ps_engage();
	return 0;
}

/* Charger dropouts under load last ~1 s. */
static void
ps_settle_arm(void)
{
	if (ps_engaged == 1 || ps_settling)
		return;
	if (!ps_settle)
		ps_settle = wl_event_loop_add_timer(event_loop, ps_settled, NULL);
	if (!ps_settle)
		return;
	ps_settling = 1;
	wl_event_source_timer_update(ps_settle, PS_SETTLE_MS);
}

static void
ps_settle_disarm(void)
{
	if (!ps_settling)
		return;
	ps_settling = 0;
	wl_event_source_timer_update(ps_settle, 0);
}

/* Battwatch snapshot changed. */
void
powersave_battery_event(void)
{
	if (on_battery_now()) {
		ps_settle_arm();
		return;
	}
	ps_settle_disarm();
	if (ps_engaged == 0)
		return;
	ps_engaged = 0;
	ps_wall_power();
}

void
powersave_init(void)
{
	/* State arrives from battwatch; nothing to poll here.  Until the
	 * first snapshot lands the machine keeps its boot defaults. */
}
