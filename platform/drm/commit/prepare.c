#include <assert.h>
#include <inttypes.h>
#include "platform/buffer/buffer.h"
#include "platform/drm/commit/commit.h"
#include "platform/drm/device/fb.h"
#include "platform/log/log.h"
#include "platform/output/output_interface.h"
#include "platform/output/commit/commit.h"
#include "platform/render/color.h"
#include "platform/render/drm_format_set.h"
#include "platform/render/drm_syncobj.h"

static const uint32_t SUPPORTED_OUTPUT_STATE =
	NX_OUTPUT_STATE_BACKEND_OPTIONAL | DRM_COMMIT_OUTPUT_STATE;

static void set_wait_point(struct nx_drm_connector_state *state,
		struct nx_drm_syncobj_timeline *timeline, uint64_t point) {
	if (timeline == NULL) {
		return;
	}
	state->wait_timeline = nx_drm_syncobj_timeline_ref(timeline);
	state->wait_point = point;
}

static struct nx_buffer *blit_to_plane(struct nx_drm_backend *drm,
		struct nx_drm_plane *plane, const struct nx_output_state *base) {
	if (!drm_plane_init_surface(plane, &drm->mgpu_renderer, base->buffer)) {
		return NULL;
	}

	bool wait = base->committed & NX_OUTPUT_STATE_WAIT_TIMELINE;
	return drm_surface_blit(&plane->mgpu_surf, base->buffer,
		wait ? base->wait_timeline : NULL, wait ? base->wait_point : 0);
}

/* Secondary GPUs scan out copies. */
static struct nx_buffer *lock_scanout_buffer(struct nx_drm_connector_state *state,
		struct nx_drm_plane *plane) {
	struct nx_drm_backend *drm = state->connector->backend;
	const struct nx_output_state *base = state->base;

	if (!drm->mgpu_renderer.nx_rend) {
		if (base->committed & NX_OUTPUT_STATE_WAIT_TIMELINE) {
			set_wait_point(state, base->wait_timeline, base->wait_point);
		}
		return nx_buffer_lock(base->buffer);
	}

	struct nx_buffer *local_buf = blit_to_plane(drm, plane, base);
	if (local_buf != NULL) {
		set_wait_point(state, plane->mgpu_surf.timeline, plane->mgpu_surf.point);
	}
	return local_buf;
}

static bool update_primary_fb(struct nx_drm_connector_state *state) {
	struct nx_drm_connector *conn = state->connector;
	assert(state->base->committed & NX_OUTPUT_STATE_BUFFER);
	assert(conn->crtc != NULL);
	assert(state->wait_timeline == NULL);

	struct nx_drm_plane *plane = conn->crtc->primary;
	struct nx_buffer *local_buf = lock_scanout_buffer(state, plane);
	if (local_buf == NULL) {
		return false;
	}

	bool ok = drm_fb_import(&state->primary_fb, conn->backend, local_buf,
		&plane->formats);
	nx_buffer_unlock(local_buf);
	if (!ok) {
		drm_conn_log(conn, NX_DEBUG,
			"Failed to import buffer for scan-out");
		return false;
	}

	output_state_get_buffer_src_box(state->base, &state->primary_viewport.src_box);
	output_state_get_buffer_dst_box(state->base, &state->primary_viewport.dst_box);
	return true;
}

static bool mgpu_can_import(struct nx_drm_connector *conn, struct nx_buffer *buffer) {
	struct nx_dmabuf_attributes dmabuf;
	if (!nx_buffer_get_dmabuf(buffer, &dmabuf)) {
		drm_conn_log(conn, NX_DEBUG, "Buffer is not a DMA-BUF");
		return false;
	}

	if (!nx_drm_format_set_has(&conn->backend->mgpu_formats, dmabuf.format, dmabuf.modifier)) {
		drm_conn_log(conn, NX_DEBUG,
			"Buffer format 0x%"PRIX32" with modifier 0x%"PRIX64" cannot be "
			"imported into multi-GPU renderer",
			dmabuf.format, dmabuf.modifier);
		return false;
	}
	return true;
}

static bool validate(const struct nx_drm_connector_state *conn_state) {
	const struct nx_output_state *state = conn_state->base;
	struct nx_drm_connector *conn = conn_state->connector;
	struct nx_output *output = &conn->output;

	uint32_t unsupported = state->committed & ~SUPPORTED_OUTPUT_STATE;
	if (unsupported != 0) {
		// ERROR: errno explains nothing here
		nx_log(NX_ERROR, "Unsupported output state fields: 0x%"PRIx32,
			unsupported);
		return false;
	}

	if ((state->committed & NX_OUTPUT_STATE_ENABLED) && state->enabled &&
			output->width == 0 && output->height == 0 &&
			!(state->committed & NX_OUTPUT_STATE_MODE)) {
		drm_conn_log(conn, NX_DEBUG,
			"Can't enable an output without a mode");
		return false;
	}

	if ((state->committed & NX_OUTPUT_STATE_ADAPTIVE_SYNC_ENABLED) &&
			state->adaptive_sync_enabled &&
			!output->adaptive_sync_supported) {
		drm_conn_log(conn, NX_DEBUG,
				"Can't enable adaptive sync: connector doesn't support VRR");
		return false;
	}

	if ((state->committed & NX_OUTPUT_STATE_BUFFER) && conn->backend->mgpu_renderer.nx_rend &&
			!mgpu_can_import(conn, state->buffer)) {
		return false;
	}

	if ((state->committed & NX_OUTPUT_STATE_COLOR_TRANSFORM) && state->color_transform != NULL &&
			state->color_transform->type != COLOR_TRANSFORM_LUT_3X1D) {
		drm_conn_log(conn, NX_DEBUG,
			"Only 3x1D LUT color transforms are supported");
		return false;
	}

	if ((state->committed & NX_OUTPUT_STATE_IMAGE_DESCRIPTION) &&
			conn->backend->iface != &drm_atomic_iface) {
		nx_log(NX_DEBUG, "Image descriptions are only supported by the atomic interface");
		return false;
	}

	return true;
}

bool drm_connector_prepare(struct nx_drm_connector_state *conn_state, bool test_only) {
	if (!validate(conn_state)) {
		return false;
	}

	const struct nx_output_state *state = conn_state->base;
	struct nx_drm_connector *conn = conn_state->connector;

	// Secondary GPUs must blit first
	if (test_only && conn->backend->mgpu_renderer.nx_rend) {
		return true;
	}

	bool has_buffer = state->committed & NX_OUTPUT_STATE_BUFFER;
	if (has_buffer && !update_primary_fb(conn_state)) {
		return false;
	}

	if (has_buffer && state->tearing_page_flip && !conn->backend->supports_tearing_page_flips) {
		nx_log(NX_ERROR, "Attempted to submit a tearing page flip to an unsupported backend!");
		return false;
	}

	if (conn_state->active && !conn_state->primary_fb) {
		drm_conn_log(conn, NX_DEBUG,
			"No primary frame buffer available for this connector");
		return false;
	}

	return true;
}
