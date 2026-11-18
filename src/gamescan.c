#include "nixlytile.h"

#define GAMESCAN_STABLE_VBLANKS 120

/* Per-vblank from rendermon while a fullscreen game is classified and
 * commits are working.  Decides only; the modeset happens next vblank
 * via gamescan_apply() at the top of rendermon, outside the commit
 * path (same deferral pattern as video_fixed_fallback_hz). */
void
gamescan_tick(Monitor *m, Client *fc, int is_direct_scanout)
{
	struct nx_surface *surf;
	int bw, bh;

	/* Not gated on gamescan_mode_active: a game changing its internal
	 * resolution again (settings change, dynamic-res step) re-runs the
	 * stability window and re-modesets; the buffer==mode check below
	 * keeps the applied mode quiet in the steady state. */
	if (!m || !fc || m->gamescan_pending)
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
	if (m->nx_output->current_mode &&
	    bw == m->nx_output->current_mode->width &&
	    bh == m->nx_output->current_mode->height) {
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
	struct nx_output_mode *target;
	struct nx_output_state state;
	struct nx_output_configuration_v1 *config;
	struct nx_output_configuration_head_v1 *config_head;

	if (!m || !m->nx_output || !m->nx_output->enabled)
		return;
	/* gamescan_mode_active does not block: a re-arm modeset (game
	 * changed internal resolution mid-session) just switches again;
	 * gamescan_original keeps pointing at the pre-game mode and
	 * restore-on-exit uses restmode() regardless. */
	if (m->gamescan_w <= 0 || m->gamescan_h <= 0)
		return;

	/* Respect a user-pinned resolution (native pin doesn't count). */
	if (monitor_mode_pinned(m->nx_output))
		return;

	target = find_mode(m->nx_output, m->gamescan_w, m->gamescan_h, 0);
	if (!target || target == m->nx_output->current_mode) {
		m->gamescan_failed_w = m->gamescan_w;
		m->gamescan_failed_h = m->gamescan_h;
		return;
	}

	if (!m->gamescan_mode_active)
		m->gamescan_original = m->nx_output->current_mode;

	nx_output_state_init(&state);
	nx_output_state_set_mode(&state, target);
	if (nx_output_test_state(m->nx_output, &state)
	    && nx_output_commit_state(m->nx_output, &state)) {
		m->gamescan_mode_active = 1;
		/* Any autolock target was picked at the old resolution —
		 * it is stale now (see autolock_apply_mode's guard). */
		m->al_mode_pending = 0;
		m->al_target_mode = NULL;
		nx_log(NX_INFO,
			"Game scanout mode: %s switched to %dx%d@%dmHz to match game buffer",
			m->nx_output->name, target->width, target->height,
			target->refresh);
		config = nx_output_configuration_v1_create();
		config_head = nx_output_configuration_head_v1_create(
				config, m->nx_output);
		config_head->state.mode = target;
		nx_output_manager_v1_set_configuration(output_mgr, config);
		updatemons(NULL, NULL);
	} else {
		m->gamescan_original = NULL;
		m->gamescan_failed_w = m->gamescan_w;
		m->gamescan_failed_h = m->gamescan_h;
	}
	nx_output_state_finish(&state);
}

void
gamescan_restore(Monitor *m)
{
	struct nx_output_mode *target;
	struct nx_output_state state;
	struct nx_output_configuration_v1 *config;
	struct nx_output_configuration_head_v1 *config_head;

	if (!m || !m->nx_output || !m->nx_output->enabled)
		return;
	m->gamescan_pending = 0;
	m->gamescan_stable = 0;
	m->gamescan_w = m->gamescan_h = 0;
	m->gamescan_failed_w = m->gamescan_failed_h = 0;
	if (!m->gamescan_mode_active)
		return;

	target = restmode(m->nx_output);
	if (!target) {
		m->gamescan_mode_active = 0;
		m->gamescan_original = NULL;
		return;
	}

	nx_output_state_init(&state);
	nx_output_state_set_mode(&state, target);
	if (nx_output_test_state(m->nx_output, &state)
	    && nx_output_commit_state(m->nx_output, &state)) {
		nx_log(NX_INFO,
			"Game scanout mode restored: %s → %dx%d@%dmHz",
			m->nx_output->name, target->width, target->height,
			target->refresh);
		config = nx_output_configuration_v1_create();
		config_head = nx_output_configuration_head_v1_create(
				config, m->nx_output);
		config_head->state.mode = target;
		nx_output_manager_v1_set_configuration(output_mgr, config);
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
	nx_output_state_finish(&state);
}
