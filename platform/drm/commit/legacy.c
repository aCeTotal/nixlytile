#include <xf86drm.h>
#include <xf86drmMode.h>
#include "platform/buffer/buffer.h"
#include "platform/drm/commit/commit.h"
#include "platform/drm/device/fb.h"
#include "platform/log/log.h"
#include "platform/output/commit/commit.h"

static bool legacy_fb_props_match(struct nx_drm_fb *fb1,
		struct nx_drm_fb *fb2) {
	struct nx_dmabuf_attributes dmabuf1 = {0}, dmabuf2 = {0};
	if (!nx_buffer_get_dmabuf(fb1->nx_buf, &dmabuf1) ||
			!nx_buffer_get_dmabuf(fb2->nx_buf, &dmabuf2)) {
		return false;
	}

	if (dmabuf1.width != dmabuf2.width ||
			dmabuf1.height != dmabuf2.height ||
			dmabuf1.format != dmabuf2.format ||
			dmabuf1.modifier != dmabuf2.modifier ||
			dmabuf1.n_planes != dmabuf2.n_planes) {
		return false;
	}

	for (int i = 0; i < dmabuf1.n_planes; i++) {
		if (dmabuf1.stride[i] != dmabuf2.stride[i] ||
				dmabuf1.offset[i] != dmabuf2.offset[i]) {
			return false;
		}
	}

	return true;
}

/* Legacy can't scale or crop. */
static bool buffer_fills_output(const struct nx_drm_connector_state *state) {
	const struct nx_output_state *base = state->base;

	int pending_width, pending_height;
	output_pending_resolution(&state->connector->output, base,
		&pending_width, &pending_height);
	if (base->buffer->width != pending_width ||
			base->buffer->height != pending_height) {
		nx_log(NX_DEBUG, "Primary buffer size mismatch");
		return false;
	}

	struct nx_fbox src_box;
	output_state_get_buffer_src_box(base, &src_box);
	if (src_box.x != 0.0 || src_box.y != 0.0 ||
			src_box.width != (double)base->buffer->width ||
			src_box.height != (double)base->buffer->height) {
		nx_log(NX_DEBUG, "Source crop not supported in DRM-legacy output");
		return false;
	}

	return true;
}

static bool legacy_crtc_test(const struct nx_drm_connector_state *state,
		bool modeset) {
	if (!(state->base->committed & NX_OUTPUT_STATE_BUFFER)) {
		return true;
	}
	if (!buffer_fills_output(state)) {
		return false;
	}
	if (modeset) {
		return true;
	}

	// Page-flips need the previous layout
	struct nx_drm_connector *conn = state->connector;
	struct nx_drm_plane *primary = conn->crtc->primary;
	struct nx_drm_fb *prev_fb = primary->queued_fb ? primary->queued_fb : primary->current_fb;
	if (prev_fb != NULL && !legacy_fb_props_match(prev_fb, state->primary_fb)) {
		drm_conn_log(conn, NX_DEBUG,
			"Cannot change scan-out buffer parameters with legacy KMS API");
		return false;
	}
	return true;
}

static bool legacy_modeset(const struct nx_drm_connector_state *state,
		uint32_t fb_id) {
	struct nx_drm_connector *conn = state->connector;
	struct nx_drm_backend *drm = conn->backend;

	uint32_t dpms = state->active ? DRM_MODE_DPMS_ON : DRM_MODE_DPMS_OFF;
	if (drmModeConnectorSetProperty(drm->fd, conn->id, conn->props.dpms,
			dpms) != 0) {
		drm_conn_log_errno(conn, NX_ERROR, "Failed to set DPMS property");
		return false;
	}

	uint32_t *conns = state->active ? &conn->id : NULL;
	int conns_len = state->active ? 1 : 0;
	drmModeModeInfo *mode = state->active ? (drmModeModeInfo *)&state->mode : NULL;
	if (drmModeSetCrtc(drm->fd, conn->crtc->id, fb_id, 0, 0,
			conns, conns_len, mode)) {
		drm_conn_log_errno(conn, NX_ERROR, "Failed to set CRTC");
		return false;
	}

	return true;
}

static bool legacy_set_vrr(const struct nx_drm_connector_state *state) {
	struct nx_drm_connector *conn = state->connector;
	struct nx_drm_crtc *crtc = conn->crtc;
	bool enabled = state->base->adaptive_sync_enabled;

	if (enabled && !conn->output.adaptive_sync_supported) {
		return false;
	}
	if (crtc->props.vrr_enabled != 0 &&
			drmModeObjectSetProperty(conn->backend->fd, crtc->id, DRM_MODE_OBJECT_CRTC,
			crtc->props.vrr_enabled, enabled) != 0) {
		drm_conn_log_errno(conn, NX_ERROR,
			"drmModeObjectSetProperty(VRR_ENABLED) failed");
		return false;
	}

	conn->output.adaptive_sync_status = enabled ?
		NX_OUTPUT_ADAPTIVE_SYNC_ENABLED : NX_OUTPUT_ADAPTIVE_SYNC_DISABLED;
	drm_conn_log(conn, NX_DEBUG, "VRR %s", enabled ? "enabled" : "disabled");
	return true;
}

static bool legacy_show_cursor(const struct nx_drm_connector_state *state) {
	struct nx_drm_connector *conn = state->connector;
	int fd = conn->backend->fd;
	uint32_t crtc_id = conn->crtc->id;

	if (state->cursor_fb == NULL) {
		drm_conn_log(conn, NX_DEBUG, "Failed to acquire cursor FB");
		return false;
	}

	drmModeFB *drm_fb = drmModeGetFB(fd, state->cursor_fb->id);
	if (drm_fb == NULL) {
		drm_conn_log_errno(conn, NX_DEBUG, "Failed to get cursor "
			"BO handle: drmModeGetFB failed");
		return false;
	}
	uint32_t cursor_handle = drm_fb->handle;
	uint32_t cursor_width = drm_fb->width;
	uint32_t cursor_height = drm_fb->height;
	drmModeFreeFB(drm_fb);

	int ret = drmModeSetCursor(fd, crtc_id, cursor_handle,
		cursor_width, cursor_height);
	int set_cursor_errno = errno;
	if (drmCloseBufferHandle(fd, cursor_handle) != 0) {
		nx_log_errno(NX_ERROR, "drmCloseBufferHandle failed");
	}
	if (ret != 0) {
		drm_conn_log(conn, NX_DEBUG, "drmModeSetCursor failed: %s",
			strerror(set_cursor_errno));
		return false;
	}

	if (drmModeMoveCursor(fd, crtc_id, conn->cursor_x, conn->cursor_y) != 0) {
		drm_conn_log_errno(conn, NX_ERROR, "drmModeMoveCursor failed");
		return false;
	}
	return true;
}

static bool legacy_set_cursor(const struct nx_drm_connector_state *state) {
	struct nx_drm_connector *conn = state->connector;
	if (conn->crtc->cursor != NULL && state->active &&
			drm_connector_is_cursor_visible(conn)) {
		return legacy_show_cursor(state);
	}

	if (drmModeSetCursor(conn->backend->fd, conn->crtc->id, 0, 0, 0)) {
		drm_conn_log_errno(conn, NX_DEBUG, "drmModeSetCursor failed");
		return false;
	}
	return true;
}

static bool legacy_crtc_commit(const struct nx_drm_connector_state *state,
		struct nx_drm_page_flip *page_flip, uint32_t flags, bool modeset) {
	struct nx_drm_connector *conn = state->connector;
	struct nx_drm_backend *drm = conn->backend;
	struct nx_drm_crtc *crtc = conn->crtc;
	const struct nx_output_state *base = state->base;

	if (state->active && state->primary_fb == NULL) {
		nx_log(NX_ERROR, "%s: failed to acquire primary FB",
			conn->output.name);
		return false;
	}
	uint32_t fb_id = state->active ? state->primary_fb->id : 0;

	if (modeset && !legacy_modeset(state, fb_id)) {
		return false;
	}
	if ((base->committed & NX_OUTPUT_STATE_COLOR_TRANSFORM) &&
			!drm_legacy_crtc_set_gamma(drm, crtc, base->color_transform)) {
		return false;
	}
	if ((base->committed & NX_OUTPUT_STATE_ADAPTIVE_SYNC_ENABLED) &&
			!legacy_set_vrr(state)) {
		return false;
	}
	if (!legacy_set_cursor(state)) {
		return false;
	}

	// No events when turning off
	if (!state->active || !(flags & DRM_MODE_PAGE_FLIP_EVENT)) {
		return true;
	}
	if (drmModePageFlip(drm->fd, crtc->id, fb_id, flags, page_flip)) {
		drm_conn_log_errno(conn, NX_ERROR, "drmModePageFlip failed");
		return false;
	}
	return true;
}

static bool legacy_commit(struct nx_drm_backend *drm,
		const struct nx_drm_device_state *state,
		struct nx_drm_page_flip *page_flip, uint32_t flags,
		bool test_only) {
	for (size_t i = 0; i < state->connectors_len; i++) {
		if (!legacy_crtc_test(&state->connectors[i], state->modeset)) {
			return false;
		}
	}

	if (test_only) {
		return true;
	}

	for (size_t i = 0; i < state->connectors_len; i++) {
		if (!legacy_crtc_commit(&state->connectors[i], page_flip, flags,
				state->modeset)) {
			return false;
		}
	}

	return true;
}

const struct nx_drm_interface drm_legacy_iface = {
	.commit = legacy_commit,
};
