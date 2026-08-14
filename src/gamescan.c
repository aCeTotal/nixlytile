#include "nixlytile.h"

/*
 * Game-resolution modeset: guarantee direct scanout on buffer/mode size
 * mismatch.
 *
 * wlroots' scene scanout election handles a scaled buffer via primary-
 * plane scaling (src/dst viewport) — AMD and Intel accept that, so a
 * 1080p game buffer on a 1440p output still scans out there.  NVIDIA's
 * planes cannot scale: every fullscreen game rendering below the
 * desktop mode falls back to GPU composition, costing latency and
 * pacing.  The bulletproof fix is the console approach (gamescope on a
 * TV, X11 exclusive fullscreen): switch the output mode to the game's
 * resolution.  Buffer == mode → scanout everywhere, and the display's
 * own scaler does the upscale for free.
 *
 * Self-selecting: only engages when a fullscreen game has presented a
 * stable off-mode buffer size for ~2 s WITHOUT direct scanout engaging
 * — on AMD/Intel plane scaling usually wins first and nothing happens.
 * The stability window also rides out menus, loading screens and
 * dynamic-resolution churn.  A size that has no matching display mode,
 * or whose modeset fails, is remembered and never retried.  No OSD is
 * shown on entry (the OSD scene node would itself block scanout).
 *
 * Restored to bestmode() on fullscreen exit/unmap, same as console
 * mode.
 */

#define GAMESCAN_STABLE_VBLANKS 120

/* Per-vblank from rendermon while a fullscreen game is classified and
 * commits are working.  Decides only; the modeset happens next vblank
 * via gamescan_apply() at the top of rendermon, outside the commit
 * path (same deferral pattern as video_fixed_fallback_hz). */
void
gamescan_tick(Monitor *m, Client *fc, int is_direct_scanout)
{
	struct wlr_surface *surf;
	int bw, bh;

	if (!m || !fc || m->gamescan_mode_active || m->gamescan_pending)
		return;
	if (is_direct_scanout) {
		m->gamescan_stable = 0;
		return;
	}
	if (fc->isspanned || m->video_mode_active || m->console_mode_active)
		return;

	surf = client_surface(fc);
	if (!surf || !surf->buffer)
		return;
	bw = surf->buffer->base.width;
	bh = surf->buffer->base.height;
	if (bw <= 0 || bh <= 0)
		return;

	/* Size matches the mode — scanout is blocked by something else
	 * (cursor, OSD, format); a modeset cannot help. */
	if (m->wlr_output->current_mode &&
	    bw == m->wlr_output->current_mode->width &&
	    bh == m->wlr_output->current_mode->height) {
		m->gamescan_stable = 0;
		return;
	}
	if (bw == m->gamescan_failed_w && bh == m->gamescan_failed_h)
		return;
	if (bw != m->gamescan_w || bh != m->gamescan_h) {
		m->gamescan_w = bw;
		m->gamescan_h = bh;
		m->gamescan_stable = 0;
		return;
	}
	if (++m->gamescan_stable < GAMESCAN_STABLE_VBLANKS)
		return;

	m->gamescan_stable = 0;
	m->gamescan_pending = 1;
}

void
gamescan_apply(Monitor *m)
{
	struct wlr_output_mode *target;
	struct wlr_output_state state;
	struct wlr_output_configuration_v1 *config;
	struct wlr_output_configuration_head_v1 *config_head;
	RuntimeMonitorConfig *rtcfg;

	if (!m || !m->wlr_output || !m->wlr_output->enabled)
		return;
	if (m->gamescan_mode_active || m->gamescan_w <= 0 || m->gamescan_h <= 0)
		return;

	/* Respect user-pinned resolution. */
	rtcfg = find_monitor_config(m->wlr_output->name);
	if (rtcfg && rtcfg->width > 0 && rtcfg->height > 0)
		return;

	target = find_mode(m->wlr_output, m->gamescan_w, m->gamescan_h, 0);
	if (!target || target == m->wlr_output->current_mode) {
		m->gamescan_failed_w = m->gamescan_w;
		m->gamescan_failed_h = m->gamescan_h;
		return;
	}

	m->gamescan_original = m->wlr_output->current_mode;

	wlr_output_state_init(&state);
	wlr_output_state_set_mode(&state, target);
	if (wlr_output_test_state(m->wlr_output, &state)
	    && wlr_output_commit_state(m->wlr_output, &state)) {
		m->gamescan_mode_active = 1;
		wlr_log(WLR_INFO,
			"Game scanout mode: %s switched to %dx%d@%dmHz to match game buffer",
			m->wlr_output->name, target->width, target->height,
			target->refresh);
		config = wlr_output_configuration_v1_create();
		config_head = wlr_output_configuration_head_v1_create(
				config, m->wlr_output);
		config_head->state.mode = target;
		wlr_output_manager_v1_set_configuration(output_mgr, config);
		updatemons(NULL, NULL);
	} else {
		m->gamescan_original = NULL;
		m->gamescan_failed_w = m->gamescan_w;
		m->gamescan_failed_h = m->gamescan_h;
	}
	wlr_output_state_finish(&state);
}

void
gamescan_restore(Monitor *m)
{
	struct wlr_output_mode *target;
	struct wlr_output_state state;
	struct wlr_output_configuration_v1 *config;
	struct wlr_output_configuration_head_v1 *config_head;

	if (!m || !m->wlr_output || !m->wlr_output->enabled)
		return;
	m->gamescan_pending = 0;
	m->gamescan_stable = 0;
	m->gamescan_w = m->gamescan_h = 0;
	m->gamescan_failed_w = m->gamescan_failed_h = 0;
	if (!m->gamescan_mode_active)
		return;

	target = bestmode(m->wlr_output);
	if (!target) {
		m->gamescan_mode_active = 0;
		m->gamescan_original = NULL;
		return;
	}

	wlr_output_state_init(&state);
	wlr_output_state_set_mode(&state, target);
	if (wlr_output_test_state(m->wlr_output, &state)
	    && wlr_output_commit_state(m->wlr_output, &state)) {
		wlr_log(WLR_INFO,
			"Game scanout mode restored: %s → %dx%d@%dmHz",
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
	m->gamescan_mode_active = 0;
	m->gamescan_original = NULL;
	wlr_output_state_finish(&state);
}
