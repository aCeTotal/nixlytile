#include "nixlytile.h"

/*
 * Auto FPS lock: lock a fullscreen game to its sustained low fps so every
 * frame gets identical screen time, and — on fixed-refresh outputs — switch
 * the display to an advertised mode whose refresh is an exact integer
 * multiple of the lock.  Under VRR no modeset is needed: the panel follows
 * the locked cadence and the lock alone gives a flat frame time.
 *
 * "Sustained low" is the rolling minimum of per-second low estimates,
 * where each second discards its worst ~5% frames — a loading hitch never
 * sets the lock, but a whole second of low fps (a real gameplay low) does.
 * The window rolls (45 s), so old lows expire.
 *
 * Menu vs gameplay: games grab the pointer (pointer-constraint) during
 * play and release it in menus/loading screens.  Once a game has been seen
 * holding a constraint, constraint-less periods are treated as menu and
 * excluded from sampling; games that never grab the pointer (strategy
 * titles) are sampled throughout.
 *
 * Raising the lock (graphics settings lowered mid-session, etc.): while
 * locked the game idles between frame_done releases, so its measured
 * interval says nothing about capability.  Instead the actual render time
 * (frame_done release → next buffer commit) is tracked; when p90 render
 * time is well under the lock interval the lock is stepped up and the
 * measurement restarts.  A raise the game cannot hold is pulled back down
 * by the normal low tracking and not retried for two minutes.
 *
 * Enforcement happens in rendermon's fps-limiter gate (output.c), which
 * treats m->al_lock_fps as the cap when the manual limiter is off.  The
 * manual limiter hotkeys always win.
 */

#define AL_RING_SECONDS        45
#define AL_WORST_SLOTS         16
#define AL_MIN_SECONDS         8                 /* warmup before first lock */
#define AL_MIN_SEC_FRAMES      10                /* ignore sparse seconds */
#define AL_DEADBAND_FPS        2
#define AL_MIN_LOCK_FPS        10
#define AL_STABLE_NS           3000000000ULL     /* candidate stable before engage */
#define AL_STABLE_DOWN_NS      1500000000ULL     /* faster confirm for lowering — a
                                                  * late release grid on a slower game
                                                  * is judder every frame it lasts */
#define AL_RAISE_INTERVAL_NS   10000000000ULL    /* min gap between raise steps */
#define AL_RAISE_HEADROOM      0.70f             /* p90 render < 70% of interval */
#define AL_RAISE_FAIL_WIN_NS   30000000000ULL    /* drop this soon after raise = failed */
#define AL_RAISE_HOLD_NS       120000000000ULL   /* don't retry a failed raise */
#define AL_MODE_STABLE_NS      15000000000ULL    /* lock stable before modeset */
#define AL_MODE_MIN_GAP_NS     90000000000ULL    /* min between autolock modesets */
#define AL_MODE_EXACT_ERR      0.005f            /* divisor match tolerance */

static int
al_gameplay_now(Monitor *m)
{
	if (active_constraint) {
		m->al_seen_constraint = 1;
		return 1;
	}
	/* Constraint released: menu/loading if this game is known to grab
	 * the pointer, normal play otherwise. */
	return !m->al_seen_constraint;
}

/* Close the current 1 s bucket: the second's sustained-low fps is the
 * k-th largest interval with k ≈ 5% of the second's frames — sub-second
 * hitches are discarded, a genuinely slow second is not. */
static void
al_close_second(Monitor *m)
{
	int k;
	float low_fps;

	if (m->al_sec_frames >= AL_MIN_SEC_FRAMES && m->al_worst_n > 0) {
		k = m->al_sec_frames / 20;
		if (k > m->al_worst_n - 1)
			k = m->al_worst_n - 1;
		low_fps = 1000000000.0f / (float)m->al_worst[k];
		m->al_low_ring[m->al_ring_idx] = low_fps;
		m->al_ring_idx = (m->al_ring_idx + 1) % AL_RING_SECONDS;
		if (m->al_ring_count < AL_RING_SECONDS)
			m->al_ring_count++;
	}
	m->al_sec_frames = 0;
	m->al_worst_n = 0;
}

/* Fed from track_game_frame_pacing with every new game buffer. */
void
autolock_sample(Monitor *m, uint64_t interval_ns, uint64_t now_ns)
{
	int i, j;

	if (!game_auto_fps_lock_enabled)
		return;

	if (!al_gameplay_now(m)) {
		/* Menu/loading: neither lows nor render times count — menu
		 * frames are cheap and would fake headroom for a raise. */
		m->al_done_sent_ns = 0;
		return;
	}

	/* Render-time measurement for the raise probe: time from the
	 * frame_done release stamped in the limiter gate to this buffer. */
	if (m->al_done_sent_ns > 0 && now_ns > m->al_done_sent_ns) {
		uint64_t render_ns = now_ns - m->al_done_sent_ns;
		if (render_ns < 200000000ULL) {
			m->al_render_ring[m->al_render_idx] = render_ns;
			m->al_render_idx = (m->al_render_idx + 1) % AL_WORST_SLOTS;
			if (m->al_render_count < AL_WORST_SLOTS)
				m->al_render_count++;
		}
	}
	m->al_done_sent_ns = 0;

	if (m->al_sec_start_ns == 0)
		m->al_sec_start_ns = now_ns;
	if (now_ns - m->al_sec_start_ns >= 1000000000ULL) {
		al_close_second(m);
		m->al_sec_start_ns = now_ns;
	}

	/* Insert into the descending worst-interval list for this second. */
	for (i = 0; i < m->al_worst_n; i++)
		if (interval_ns > m->al_worst[i])
			break;
	if (i < AL_WORST_SLOTS) {
		int last = m->al_worst_n < AL_WORST_SLOTS
			? m->al_worst_n : AL_WORST_SLOTS - 1;
		for (j = last; j > i; j--)
			m->al_worst[j] = m->al_worst[j - 1];
		m->al_worst[i] = interval_ns;
		if (m->al_worst_n < AL_WORST_SLOTS)
			m->al_worst_n++;
	}
	m->al_sec_frames++;
}

static int
al_candidate(Monitor *m)
{
	float min_fps = 0.0f;
	int i, fps;

	if (m->al_ring_count < AL_MIN_SECONDS)
		return 0;
	for (i = 0; i < m->al_ring_count; i++)
		if (min_fps == 0.0f || m->al_low_ring[i] < min_fps)
			min_fps = m->al_low_ring[i];
	fps = (int)min_fps;
	if (fps < AL_MIN_LOCK_FPS)
		fps = AL_MIN_LOCK_FPS;
	return fps;
}

static uint64_t
al_render_p90(Monitor *m)
{
	uint64_t sorted[AL_WORST_SLOTS];
	int i, j, n = m->al_render_count;
	uint64_t v;

	for (i = 0; i < n; i++)
		sorted[i] = m->al_render_ring[i];
	for (i = 1; i < n; i++) {
		v = sorted[i];
		for (j = i; j > 0 && sorted[j - 1] > v; j--)
			sorted[j] = sorted[j - 1];
		sorted[j] = v;
	}
	return sorted[n * 9 / 10];
}

static int
al_max_display_fps(Monitor *m)
{
	struct wlr_output_mode *mode;
	int max_hz = 0;

	wl_list_for_each(mode, &m->wlr_output->modes, link)
		if (mode->refresh / 1000 > max_hz)
			max_hz = mode->refresh / 1000;
	if (max_hz == 0 && m->wlr_output->current_mode)
		max_hz = m->wlr_output->current_mode->refresh / 1000;
	return max_hz;
}

/* Lock slightly BELOW the measured sustained low: at the exact low the
 * game has zero slack and its slowest ~5% of frames miss their release
 * slot — a doubled frame time several times a second.  ~3% margin costs
 * a couple of fps and buys a pace where virtually every frame makes its
 * slot. */
static int
al_margined(int cand)
{
	int margin = cand / 32;

	if (margin < 1)
		margin = 1;
	cand -= margin;
	if (cand < AL_MIN_LOCK_FPS)
		cand = AL_MIN_LOCK_FPS;
	return cand;
}

static void
al_set_lock(Monitor *m, int fps, uint64_t now_ns, const char *why)
{
	wlr_log(WLR_INFO, "Auto FPS lock on %s: %d → %d FPS (%s)",
		m->wlr_output->name, m->al_lock_fps, fps, why);
	m->al_lock_fps = fps;
	m->al_lock_since_ns = now_ns;
	m->al_candidate_fps = 0;
	m->al_candidate_since_ns = 0;
}

/* Pick an advertised mode at the current resolution whose refresh divides
 * exactly to the lock.  Sets al_mode_pending for the deferred modeset at
 * the top of rendermon (same pattern as gamescan). */
static void
al_consider_modeset(Monitor *m, uint64_t now_ns)
{
	struct wlr_output_mode *mode, *cur, *best = NULL;
	RuntimeMonitorConfig *rtcfg;
	float hz, eff, err;
	int n, lock = m->al_lock_fps;

	cur = m->wlr_output->current_mode;
	if (!cur || lock <= 0)
		return;
	if (now_ns - m->al_lock_since_ns < AL_MODE_STABLE_NS)
		return;
	if (m->al_last_modeset_ns > 0 &&
	    now_ns - m->al_last_modeset_ns < AL_MODE_MIN_GAP_NS)
		return;

	/* Respect a user-pinned refresh rate. */
	rtcfg = find_monitor_config(m->wlr_output->name);
	if (rtcfg && rtcfg->refresh > 0)
		return;

	/* Current mode already an exact multiple → nothing to gain. */
	hz = (float)cur->refresh / 1000.0f;
	n = (int)roundf(hz / (float)lock);
	if (n >= 1 && fabsf(hz / (float)n - (float)lock) / (float)lock
			<= AL_MODE_EXACT_ERR)
		return;

	wl_list_for_each(mode, &m->wlr_output->modes, link) {
		if (mode->width != cur->width || mode->height != cur->height)
			continue;
		if (mode == m->al_failed_mode)
			continue;
		hz = (float)mode->refresh / 1000.0f;
		n = (int)roundf(hz / (float)lock);
		if (n < 1)
			continue;
		eff = hz / (float)n;
		err = fabsf(eff - (float)lock) / (float)lock;
		if (err > AL_MODE_EXACT_ERR)
			continue;
		if (!best || mode->refresh > best->refresh)
			best = mode;
	}
	if (best && best != cur) {
		m->al_target_mode = best;
		m->al_mode_pending = 1;
	}
}

/* Per vblank from rendermon while a fullscreen game is classified. */
void
autolock_tick(Monitor *m, int allow_tearing, uint64_t now_ns)
{
	int cand;

	if (!game_auto_fps_lock_enabled || allow_tearing)
		return;
	/* Manual limiter hotkeys own the cap. */
	if (fps_limit_enabled)
		return;

	/* A stalled second (game froze, tab-away) never reaches
	 * autolock_sample — close it from here so the window keeps rolling. */
	if (m->al_sec_start_ns > 0 &&
	    now_ns - m->al_sec_start_ns >= 2000000000ULL) {
		al_close_second(m);
		m->al_sec_start_ns = now_ns;
	}

	cand = al_candidate(m);
	if (cand <= 0)
		return;

	/* Candidate stability aging with a deadband. */
	if (m->al_candidate_fps == 0 ||
	    abs(cand - m->al_candidate_fps) > AL_DEADBAND_FPS) {
		m->al_candidate_fps = cand;
		m->al_candidate_since_ns = now_ns;
	}

	/* Effective release rate under the current lock: on fixed refresh
	 * the enforcement ceil-snaps to a divisor at or below the lock
	 * (lock 60 on 144 Hz → 48 releases).  A game saturating every
	 * release slot is meeting the cap, not showing a real low — its
	 * throttled lows must not drag the lock down to the divisor before
	 * the modeset (144 → 120 for an exact 60-multiple) gets its
	 * stability window.  Only lows below the effective rate count. */
	float eff_fps = (float)m->al_lock_fps;
	if (m->al_lock_fps > 0 && !m->game_vrr_active) {
		float hz = 0.0f;
		if (m->present_interval_ns > 0)
			hz = 1000000000.0f / (float)m->present_interval_ns;
		else if (m->wlr_output->current_mode)
			hz = (float)m->wlr_output->current_mode->refresh / 1000.0f;
		if (hz >= 30.0f) {
			int n = (int)ceilf(hz / (float)m->al_lock_fps - 0.02f);
			if (n < 1)
				n = 1;
			eff_fps = hz / (float)n;
		}
	}

	if (m->al_lock_fps == 0) {
		if (now_ns - m->al_candidate_since_ns >= AL_STABLE_NS)
			al_set_lock(m, al_margined(m->al_candidate_fps),
				now_ns, "engage");
	} else if ((float)m->al_candidate_fps < eff_fps - (float)AL_DEADBAND_FPS) {
		if (now_ns - m->al_candidate_since_ns >= AL_STABLE_DOWN_NS) {
			/* Dropping right after a raise = the raise failed;
			 * remember it so the probe doesn't oscillate. */
			if (m->al_last_raise_ns > 0 &&
			    now_ns - m->al_last_raise_ns < AL_RAISE_FAIL_WIN_NS) {
				m->al_failed_raise_fps = m->al_lock_fps;
				m->al_failed_raise_ns = now_ns;
			}
			al_set_lock(m, al_margined(m->al_candidate_fps),
				now_ns, "sustained low");
		}
	} else if (al_gameplay_now(m) &&
	           (m->al_last_raise_ns == 0 ||
	            now_ns - m->al_last_raise_ns >= AL_RAISE_INTERVAL_NS) &&
	           m->al_render_count >= AL_WORST_SLOTS / 2) {
		/* Headroom probe: game finishes frames well inside the lock
		 * interval → capability rose (settings change etc.). */
		uint64_t interval = 1000000000ULL / (uint64_t)m->al_lock_fps;
		uint64_t p90 = al_render_p90(m);
		if (p90 > 0 && p90 < (uint64_t)(AL_RAISE_HEADROOM * interval)) {
			/* p90 render time is a direct capability estimate (it
			 * even overestimates by up to a vblank, since buffer
			 * arrival is vblank-quantized) — jump to ~90% of it in
			 * one step instead of creeping in fixed increments.
			 * The low tracking and the failed-raise clamp catch an
			 * overshoot. */
			int est = (int)(0.9f * (1000000000.0f / (float)p90));
			int step = m->al_lock_fps / 8;
			int target = m->al_lock_fps + (step > 2 ? step : 2);
			int max_fps = al_max_display_fps(m);
			if (est > target)
				target = est;
			if (max_fps > 0 && target > max_fps)
				target = max_fps;
			if (m->al_failed_raise_ns > 0 &&
			    now_ns - m->al_failed_raise_ns < AL_RAISE_HOLD_NS &&
			    target >= m->al_failed_raise_fps)
				target = m->al_failed_raise_fps - AL_DEADBAND_FPS - 1;
			if (target > m->al_lock_fps) {
				al_set_lock(m, target, now_ns, "headroom probe");
				m->al_last_raise_ns = now_ns;
				/* Old throttled seconds would read as "sustained
				 * low" at the previous lock — measure fresh. */
				m->al_ring_count = 0;
				m->al_ring_idx = 0;
				m->al_render_count = 0;
				m->al_render_idx = 0;
			}
		}
	}

	if (m->al_lock_fps > 0 && !m->game_vrr_active)
		al_consider_modeset(m, now_ns);
}

/* Deferred from the top of rendermon, outside the commit path. */
void
autolock_apply_mode(Monitor *m)
{
	struct wlr_output_mode *target;
	struct wlr_output_state state;
	struct wlr_output_configuration_v1 *config;
	struct wlr_output_configuration_head_v1 *config_head;

	if (!m || !m->wlr_output || !m->wlr_output->enabled)
		return;
	target = m->al_target_mode;
	m->al_target_mode = NULL;
	if (!target || target == m->wlr_output->current_mode)
		return;
	wlr_output_state_init(&state);
	wlr_output_state_set_mode(&state, target);
	if (wlr_output_test_state(m->wlr_output, &state)
	    && wlr_output_commit_state(m->wlr_output, &state)) {
		m->al_mode_active = 1;
		m->al_last_modeset_ns = get_time_ns();
		wlr_log(WLR_INFO,
			"Auto FPS lock: %s switched to %dx%d@%dmHz (exact multiple of %d FPS)",
			m->wlr_output->name, target->width, target->height,
			target->refresh, m->al_lock_fps);
		config = wlr_output_configuration_v1_create();
		config_head = wlr_output_configuration_head_v1_create(
				config, m->wlr_output);
		config_head->state.mode = target;
		wlr_output_manager_v1_set_configuration(output_mgr, config);
		updatemons(NULL, NULL);
	} else {
		m->al_failed_mode = target;
	}
	wlr_output_state_finish(&state);
}

/* Fullscreen enter/exit: clear all state; restore the refresh if we
 * switched it and no other mode owner (gamescan/console) will. */
void
autolock_reset(Monitor *m)
{
	struct wlr_output_mode *target;
	struct wlr_output_state state;
	struct wlr_output_configuration_v1 *config;
	struct wlr_output_configuration_head_v1 *config_head;
	int restore;

	if (!m)
		return;
	restore = m->al_mode_active && !m->gamescan_mode_active
		&& !m->console_mode_active;

	m->al_sec_start_ns = 0;
	m->al_worst_n = 0;
	m->al_sec_frames = 0;
	m->al_ring_idx = 0;
	m->al_ring_count = 0;
	m->al_seen_constraint = 0;
	m->al_lock_fps = 0;
	m->al_lock_since_ns = 0;
	m->al_candidate_fps = 0;
	m->al_candidate_since_ns = 0;
	m->al_done_sent_ns = 0;
	m->al_render_idx = 0;
	m->al_render_count = 0;
	m->al_last_raise_ns = 0;
	m->al_failed_raise_fps = 0;
	m->al_failed_raise_ns = 0;
	m->al_mode_pending = 0;
	m->al_target_mode = NULL;
	m->al_failed_mode = NULL;
	m->al_mode_active = 0;
	m->al_last_modeset_ns = 0;

	if (!restore || !m->wlr_output || !m->wlr_output->enabled)
		return;
	target = bestmode(m->wlr_output);
	if (!target || target == m->wlr_output->current_mode)
		return;

	wlr_output_state_init(&state);
	wlr_output_state_set_mode(&state, target);
	if (wlr_output_test_state(m->wlr_output, &state)
	    && wlr_output_commit_state(m->wlr_output, &state)) {
		wlr_log(WLR_INFO, "Auto FPS lock: %s restored to %dx%d@%dmHz",
			m->wlr_output->name, target->width, target->height,
			target->refresh);
		config = wlr_output_configuration_v1_create();
		config_head = wlr_output_configuration_head_v1_create(
				config, m->wlr_output);
		config_head->state.mode = target;
		wlr_output_manager_v1_set_configuration(output_mgr, config);
		{
			char osd_msg[64];
			snprintf(osd_msg, sizeof(osd_msg), "%dx%d @ %d Hz",
				target->width, target->height,
				target->refresh / 1000);
			show_hz_osd(m, osd_msg);
		}
		updatemons(NULL, NULL);
	}
	wlr_output_state_finish(&state);
}
