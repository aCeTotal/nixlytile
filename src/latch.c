#include "nixlytile.h"

/*
 * Late-latch frame scheduling for fullscreen games (gamescope-style).
 *
 * By default rendermon builds and commits immediately when the frame
 * event fires — right after the previous vblank.  The committed frame
 * then waits almost a full refresh before it is scanned out, and any
 * game buffer arriving during that wait misses the flip: up to one
 * frame of avoidable latency.
 *
 * Instead, defer the build+commit to just before the predicted next
 * present: deadline = target_present_ns (already includes the adaptive
 * commit margin) minus a rolling draw-time estimate minus a fixed
 * redzone.  The newest game buffer is latched as late as possible,
 * exactly like gamescope's vblankmanager (rollingMaxDrawTime + redzone
 * before vblank).
 *
 * The draw-time estimate is an asymmetric sawtooth: it spikes up
 * instantly on a slow frame and decays slowly (98 %/frame), so one
 * hitch budgets the following frames conservatively instead of causing
 * a run of missed flips.  Composited frames (no direct scanout) get an
 * extra fixed floor since GPU render completion isn't measured.
 *
 * Only for fixed-refresh operation: under VRR the flip lands whenever
 * the game commits, and tearing flips are immediate — deferring would
 * only add latency there.  Misses are safe: a stale or near deadline
 * falls through to the normal immediate path.
 */

int game_late_latch_enabled = 1;

#define LATCH_REDZONE_NS     1650000ULL /* gamescope kDefaultVBlankRedZone */
#define LATCH_COMPOSITE_NS   2400000ULL /* gamescope kDefaultVBlankDrawTimeMinCompositing */
#define LATCH_START_DRAW_NS  3000000ULL /* gamescope kStartingVBlankDrawTime */
#define LATCH_MIN_DRAW_NS     200000ULL
#define LATCH_MAX_DRAW_NS   10000000ULL

static int
latch_timer_cb(void *data)
{
	Monitor *m = data;

	m->latch_armed = 0;
	m->latch_fired = 1;
	rendermon(&m->frame, NULL);
	m->latch_fired = 0;
	return 0;
}

/* Sawtooth build+commit time estimate: instant spike-up, 98 % decay. */
void
latch_track_draw(Monitor *m, uint64_t draw_ns)
{
	if (draw_ns < LATCH_MIN_DRAW_NS)
		draw_ns = LATCH_MIN_DRAW_NS;
	if (draw_ns > LATCH_MAX_DRAW_NS)
		draw_ns = LATCH_MAX_DRAW_NS;
	if (m->rolling_draw_ns == 0)
		m->rolling_draw_ns = LATCH_START_DRAW_NS;
	if (draw_ns > m->rolling_draw_ns)
		m->rolling_draw_ns = draw_ns;
	else
		m->rolling_draw_ns = (m->rolling_draw_ns * 98 + draw_ns * 2) / 100;
}

/* Called from rendermon after content classification.  Returns 1 when
 * this vblank's build+commit has been deferred to the latch timer and
 * the caller must return immediately. */
int
latch_defer_frame(Monitor *m, int is_game, int allow_tearing, uint64_t now_ns)
{
	uint64_t lead, deadline;

	if (!game_late_latch_enabled || !is_game || m->latch_fired)
		return 0;
	if (m->vrr_active || m->game_vrr_active || allow_tearing)
		return 0;
	if (m->present_interval_ns == 0 || m->target_present_ns <= now_ns)
		return 0;

	lead = (m->rolling_draw_ns ? m->rolling_draw_ns : LATCH_START_DRAW_NS)
		+ LATCH_REDZONE_NS;
	if (!m->direct_scanout_active)
		lead += LATCH_COMPOSITE_NS;

	deadline = m->target_present_ns;
	/* 100 µs floor: a deadline closer than that gains nothing over the
	 * immediate path. */
	if (deadline <= now_ns + lead + 100000ULL)
		return 0;

	if (!m->latch_timer)
		m->latch_timer = nstimer_create(latch_timer_cb, m);
	if (!m->latch_timer)
		return 0;

	nstimer_arm_abs(m->latch_timer, deadline - lead);
	m->latch_armed = 1;
	return 1;
}
