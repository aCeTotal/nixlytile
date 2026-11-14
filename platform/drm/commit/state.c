#include <assert.h>
#include "platform/drm/commit/commit.h"
#include "platform/drm/device/fb.h"
#include "platform/output/commit/commit.h"
#include "platform/render/drm_syncobj.h"

/* Requested mode, else custom timings. */
static void init_mode(struct nx_drm_connector_state *state) {
	const struct nx_output_state *base = state->base;
	struct nx_output *output = &state->connector->output;

	struct nx_output_mode *mode = output->current_mode;
	int32_t width = output->width;
	int32_t height = output->height;
	int32_t refresh = output->refresh;

	if (base->committed & NX_OUTPUT_STATE_MODE) {
		switch (base->mode_type) {
		case NX_OUTPUT_STATE_MODE_FIXED:
			mode = base->mode;
			break;
		case NX_OUTPUT_STATE_MODE_CUSTOM:
			mode = NULL;
			width = base->custom_mode.width;
			height = base->custom_mode.height;
			refresh = base->custom_mode.refresh;
			break;
		}
	}

	if (mode) {
		struct nx_drm_mode *drm_mode = wl_container_of(mode, drm_mode, nx_mode);
		state->mode = drm_mode->drm_mode;
		return;
	}

	if (!drm_scaled_edid_mode(output, &state->mode, width, height, refresh)) {
		drm_generate_cvt_mode(&state->mode, width, height, (float)refresh / 1000);
	}
	state->mode.type = DRM_MODE_TYPE_USERDEF;
}

static struct nx_drm_fb *lock_newest(struct nx_drm_fb *newer, struct nx_drm_fb *older) {
	struct nx_drm_fb *fb = newer != NULL ? newer : older;
	return fb != NULL ? drm_fb_lock(fb) : NULL;
}

/* Keeps showing the latest buffers. */
static void lock_current_fbs(struct nx_drm_connector_state *state) {
	struct nx_drm_connector *conn = state->connector;
	assert(conn->crtc != NULL);

	struct nx_drm_plane *primary = conn->crtc->primary;
	state->primary_fb = lock_newest(primary->queued_fb, primary->current_fb);
	if (state->primary_fb != NULL) {
		state->primary_viewport = primary->viewport;
	}

	if (!conn->cursor_enabled) {
		return;
	}

	struct nx_drm_plane *cursor = conn->crtc->cursor;
	assert(cursor != NULL);
	struct nx_drm_fb *pending = conn->cursor_pending_fb != NULL ?
		conn->cursor_pending_fb : cursor->queued_fb;
	state->cursor_fb = lock_newest(pending, cursor->current_fb);
}

void drm_connector_state_init(struct nx_drm_connector_state *state,
		struct nx_drm_connector *conn,
		const struct nx_output_state *base) {
	*state = (struct nx_drm_connector_state){
		.connector = conn,
		.base = base,
		.active = output_pending_enabled(&conn->output, base),
		.primary_in_fence_fd = -1,
	};

	init_mode(state);
	if (state->active) {
		lock_current_fbs(state);
	}
}

void drm_connector_state_finish(struct nx_drm_connector_state *state) {
	drm_fb_clear(&state->primary_fb);
	drm_fb_clear(&state->cursor_fb);
	nx_drm_syncobj_timeline_unref(state->wait_timeline);
}
