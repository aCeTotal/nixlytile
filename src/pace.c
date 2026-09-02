#include "nixlytile.h"

/*
 * Paced virtual refresh modes.
 *
 * Some panels accept no pixel clock beyond their fixed EDID modes (i915
 * eDP DRRS panels: exactly the EDID clocks work, everything between is
 * rejected in the atomic test), so intermediate refresh rates can't be
 * had as real scanout modes.  Instead, register virtual modes at even
 * divisors of the fastest native rate (300 Hz → 150/100/75): the mode
 * commits the native scanout timings and the compositor flips only
 * every Nth vblank.  The panel quantizes each flip to its vblank grid,
 * so the cadence is exactly even — no judder, no VRR flicker — while
 * clients see an ordinary mode and the GPU renders at the paced rate.
 *
 * Registration: pace_register_modes() (createmon + requestmonstate,
 * idempotent).  Enforcement: pace_defer_frame() from rendermon defers
 * the build+commit to just before the Nth vblank after the last
 * present, same deferral pattern as latch.c.
 */

#define PACE_MIN_BASE_MHZ 200000 /* only panels ≥200 Hz get divisor modes */
#define PACE_MAX_DIVISOR  4

void
pace_register_modes(struct wlr_output *output)
{
	struct wlr_output_mode *mode, *top = NULL;
	int div;

	if (!wlr_output_is_drm(output))
		return;
	wl_list_for_each(mode, &output->modes, link) {
		if (wlr_drm_connector_mode_pace_divisor(output, mode))
			continue;
		if (!top || mode->refresh > top->refresh)
			top = mode;
	}
	if (!top || top->refresh < PACE_MIN_BASE_MHZ)
		return;
	for (div = 2; div <= PACE_MAX_DIVISOR; div++) {
		int target = top->refresh / div;
		int have = 0;

		wl_list_for_each(mode, &output->modes, link)
			if (mode->width == top->width &&
					mode->height == top->height &&
					abs(mode->refresh - target) < 2000) {
				have = 1;
				break;
			}
		if (!have)
			wlr_drm_connector_add_paced_mode(output, top, div);
	}
}

static int
pace_timer_cb(void *data)
{
	Monitor *m = data;

	m->pace_armed = 0;
	m->pace_fired = 1;
	rendermon(&m->frame, NULL);
	m->pace_fired = 0;
	return 0;
}

/* Called from rendermon after content classification.  Returns 1 when
 * this frame's build+commit has been deferred to the commit window
 * before the Nth vblank and the caller must return immediately. */
int
pace_defer_frame(Monitor *m, int allow_tearing, uint64_t now_ns)
{
	uint64_t vb_ns, target;
	int div, delay_ms;

	if (m->pace_fired || allow_tearing)
		return 0;
	div = wlr_drm_connector_mode_pace_divisor(m->wlr_output,
			m->wlr_output->current_mode);
	if (div < 2 || m->last_present_ns == 0 ||
			m->wlr_output->current_mode->refresh <= 0)
		return 0;

	/* Underlying scanout vblank interval: paced refresh × divisor. */
	vb_ns = 1000000000000ULL /
		((uint64_t)m->wlr_output->current_mode->refresh * div);

	/* The flip must land on the Nth vblank after the last present: any
	 * commit inside ((N-1)·vb, N·vb) does, since the fixed scanout
	 * quantizes it.  Aim half a vblank before the target vblank —
	 * ms-granular timers firing up to 1 ms early still land inside the
	 * window.  A missed window (heavy frame, idle wake) just commits
	 * immediately and re-anchors on its own present event. */
	target = m->last_present_ns + (uint64_t)div * vb_ns - vb_ns / 2;
	if (now_ns + 1000000ULL >= target)
		return 0;
	delay_ms = (int)((target - now_ns) / 1000000ULL);
	if (delay_ms < 1)
		return 0;

	if (!m->pace_timer)
		m->pace_timer = wl_event_loop_add_timer(event_loop, pace_timer_cb, m);
	if (!m->pace_timer)
		return 0;

	wl_event_source_timer_update(m->pace_timer, delay_ms);
	m->pace_armed = 1;
	return 1;
}
