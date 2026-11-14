#include <assert.h>
#include "platform/drm/commit/commit.h"
#include "platform/drm/device/fb.h"
#include "platform/log/log.h"
#include "platform/output/commit/commit.h"
#include "platform/render/drm_syncobj.h"
#include "platform/session/session.h"

static bool is_paced(const struct nx_output_state *base) {
	if (base->mode_type != NX_OUTPUT_STATE_MODE_FIXED || base->mode == NULL) {
		return false;
	}
	struct nx_drm_mode *drm_mode = wl_container_of(base->mode, drm_mode, nx_mode);
	return drm_mode->pace_divisor >= 2;
}

static void queue_primary(const struct nx_drm_connector_state *state,
		struct nx_drm_plane *primary) {
	drm_fb_copy(&primary->queued_fb, state->primary_fb);
	drm_plane_release(&primary->queued_release_timeline, &primary->queued_release_point);
	if (state->base->signal_timeline != NULL) {
		primary->queued_release_timeline = nx_drm_syncobj_timeline_ref(state->base->signal_timeline);
		primary->queued_release_point = state->base->signal_point;
	}
	primary->viewport = state->primary_viewport;
}

/* Legacy disables send no event. */
static void drop_legacy_disable_flip(struct nx_drm_connector *conn,
		uint32_t crtc_id, struct nx_drm_page_flip *page_flip) {
	if (page_flip == NULL || conn->backend->iface != &drm_legacy_iface) {
		return;
	}
	drm_page_flip_pop(page_flip, crtc_id);
	conn->pending_page_flip = NULL;
	if (page_flip->connectors_len == 0) {
		drm_page_flip_destroy(page_flip);
	}
}

static void apply_commit(const struct nx_drm_connector_state *state,
		struct nx_drm_page_flip *page_flip) {
	struct nx_drm_connector *conn = state->connector;
	struct nx_drm_crtc *crtc = conn->crtc;

	queue_primary(state, crtc->primary);
	if (crtc->cursor != NULL) {
		drm_fb_copy(&crtc->cursor->queued_fb, state->cursor_fb);
	}
	drm_fb_clear(&conn->cursor_pending_fb);

	drm_connector_set_pending_page_flip(conn, page_flip);

	// Paced modes report flip rate
	if (state->base->committed & NX_OUTPUT_STATE_MODE) {
		conn->refresh = is_paced(state->base) ? state->base->mode->refresh :
			drm_calculate_refresh_rate(&state->mode);
	}

	if (state->active) {
		return;
	}

	drm_plane_finish_surface(crtc->primary);
	drm_plane_finish_surface(crtc->cursor);
	drm_fb_clear(&conn->cursor_pending_fb);

	conn->cursor_enabled = false;
	conn->crtc = NULL;

	drop_legacy_disable_flip(conn, crtc->id, page_flip);
}

static bool drm_commit(struct nx_drm_backend *drm,
		const struct nx_drm_device_state *state,
		uint32_t flags, bool test_only) {
	// The interface adds atomic-only flags
	assert((flags & ~DRM_MODE_PAGE_FLIP_FLAGS) == 0);

	struct nx_drm_page_flip *page_flip = NULL;
	if (flags & DRM_MODE_PAGE_FLIP_EVENT) {
		page_flip = drm_page_flip_create(drm, state);
		if (page_flip == NULL) {
			return false;
		}
		page_flip->async = (flags & DRM_MODE_PAGE_FLIP_ASYNC);
	}

	bool ok = drm->iface->commit(drm, state, page_flip, flags, test_only);
	if (!ok || test_only) {
		drm_page_flip_destroy(page_flip);
		return ok;
	}

	for (size_t i = 0; i < state->connectors_len; i++) {
		apply_commit(&state->connectors[i], page_flip);
	}
	return true;
}

static void log_modeset(const struct nx_drm_connector_state *pending) {
	struct nx_drm_connector *conn = pending->connector;
	if (!pending->active) {
		drm_conn_log(conn, NX_INFO, "Turning off");
		return;
	}
	drm_conn_log(conn, NX_INFO, "Modesetting with %dx%d @ %.3f Hz",
		pending->mode.hdisplay, pending->mode.vdisplay,
		(float)drm_calculate_refresh_rate(&pending->mode) / 1000);
}

static bool commit_pending(const struct nx_drm_device_state *dev, bool test_only) {
	struct nx_drm_connector_state *pending = &dev->connectors[0];
	struct nx_drm_connector *conn = pending->connector;

	if (!drm_connector_prepare(pending, test_only)) {
		return false;
	}

	// Secondary GPUs must blit first
	if (test_only && conn->backend->mgpu_renderer.nx_rend) {
		return true;
	}

	// Already disabled
	if (!pending->active && conn->crtc == NULL) {
		return true;
	}

	if (!test_only && dev->modeset) {
		log_modeset(pending);
	}

	// Modesets wait for queued flips
	if (!test_only && dev->nonblock && conn->pending_page_flip != NULL) {
		drm_conn_log(conn, NX_ERROR, "Failed to page-flip output: "
			"a page-flip is already pending");
		return false;
	}

	uint32_t flags = 0;
	if (!test_only && pending->active) {
		flags |= DRM_MODE_PAGE_FLIP_EVENT;
	}
	if (pending->base->tearing_page_flip) {
		flags |= DRM_MODE_PAGE_FLIP_ASYNC;
	}

	return drm_commit(conn->backend, dev, flags, test_only);
}

bool drm_connector_commit_state(struct nx_drm_connector *conn,
		const struct nx_output_state *state, bool test_only) {
	if (!conn->backend->session->active) {
		return false;
	}

	// Nothing for KMS to test
	if (test_only && (state->committed & DRM_COMMIT_OUTPUT_STATE) == 0) {
		return true;
	}

	if (output_pending_enabled(&conn->output, state) && !drm_connector_alloc_crtc(conn)) {
		drm_conn_log(conn, NX_DEBUG,
			"No CRTC available for this connector");
		return false;
	}

	struct nx_drm_connector_state pending = {0};
	drm_connector_state_init(&pending, conn, state);
	struct nx_drm_device_state pending_dev = {
		.modeset = state->allow_reconfiguration,
		// Bufferless commits block, else EBUSY
		.nonblock = !state->allow_reconfiguration &&
			(state->committed & NX_OUTPUT_STATE_BUFFER),
		.connectors = &pending,
		.connectors_len = 1,
	};

	bool ok = commit_pending(&pending_dev, test_only);
	drm_connector_state_finish(&pending);
	return ok;
}
