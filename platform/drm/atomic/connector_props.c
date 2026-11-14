#include <drm_fourcc.h>
#include <inttypes.h>
#include "platform/buffer/buffer.h"
#include "platform/drm/atomic/atomic.h"
#include "platform/drm/device/fb.h"
#include "platform/log/log.h"

static uint64_t max_bpc_for_format(uint32_t format) {
	switch (format) {
	case DRM_FORMAT_XRGB2101010:
	case DRM_FORMAT_ARGB2101010:
	case DRM_FORMAT_XBGR2101010:
	case DRM_FORMAT_ABGR2101010:
		return 10;
	case DRM_FORMAT_XBGR16161616F:
	case DRM_FORMAT_ABGR16161616F:
	case DRM_FORMAT_XBGR16161616:
	case DRM_FORMAT_ABGR16161616:
		return 16;
	default:
		return 8;
	}
}

static uint64_t pick_max_bpc(struct nx_drm_connector *conn, struct nx_drm_fb *fb) {
	uint32_t format = DRM_FORMAT_INVALID;
	struct nx_dmabuf_attributes attribs = {0};
	if (nx_buffer_get_dmabuf(fb->nx_buf, &attribs)) {
		format = attribs.format;
	}

	uint64_t target_bpc = max_bpc_for_format(format);
	if (target_bpc < conn->max_bpc_bounds[0]) {
		target_bpc = conn->max_bpc_bounds[0];
	}
	if (target_bpc > conn->max_bpc_bounds[1]) {
		target_bpc = conn->max_bpc_bounds[1];
	}
	return target_bpc;
}

static uint64_t to_fixed_16_16(double value) {
	return (uint64_t)(value * (1 << 16));
}

static void plane_disable(struct drm_atomic *atom, struct nx_drm_plane *plane) {
	drm_atomic_add(atom, plane->id, plane->props.fb_id, 0);
	drm_atomic_add(atom, plane->id, plane->props.crtc_id, 0);
}

static void set_plane_props(struct drm_atomic *atom, struct nx_drm_plane *plane,
		struct nx_drm_fb *fb, uint32_t crtc_id,
		const struct nx_drm_viewport *viewport) {
	uint32_t id = plane->id;
	const struct nx_drm_plane_props *props = &plane->props;

	if (fb == NULL) {
		nx_log(NX_ERROR, "Failed to acquire FB for plane %"PRIu32, plane->id);
		atom->failed = true;
		return;
	}

	drm_atomic_add(atom, id, props->src_x, to_fixed_16_16(viewport->src_box.x));
	drm_atomic_add(atom, id, props->src_y, to_fixed_16_16(viewport->src_box.y));
	drm_atomic_add(atom, id, props->src_w, to_fixed_16_16(viewport->src_box.width));
	drm_atomic_add(atom, id, props->src_h, to_fixed_16_16(viewport->src_box.height));
	drm_atomic_add(atom, id, props->fb_id, fb->id);
	drm_atomic_add(atom, id, props->crtc_id, crtc_id);
	drm_atomic_add(atom, id, props->crtc_x, viewport->dst_box.x);
	drm_atomic_add(atom, id, props->crtc_y, viewport->dst_box.y);
	drm_atomic_add(atom, id, props->crtc_w, viewport->dst_box.width);
	drm_atomic_add(atom, id, props->crtc_h, viewport->dst_box.height);
}

static void add_connector_props(struct drm_atomic *atom,
		const struct nx_drm_connector_state *state, bool modeset) {
	struct nx_drm_connector *conn = state->connector;
	const struct nx_drm_connector_props *props = &conn->props;
	bool active = state->active;

	drm_atomic_add(atom, conn->id, props->crtc_id, active ? conn->crtc->id : 0);
	if (modeset && active && props->link_status != 0) {
		drm_atomic_add(atom, conn->id, props->link_status,
			DRM_MODE_LINK_STATUS_GOOD);
	}
	if (active && props->content_type != 0) {
		drm_atomic_add(atom, conn->id, props->content_type, state->content_type);
	}
	if (modeset && active && props->max_bpc != 0 && conn->max_bpc_bounds[1] != 0) {
		drm_atomic_add(atom, conn->id, props->max_bpc, pick_max_bpc(conn, state->primary_fb));
	}
	if (props->colorspace != 0) {
		drm_atomic_add(atom, conn->id, props->colorspace, state->colorspace);
	}
	if (props->hdr_output_metadata != 0) {
		drm_atomic_add(atom, conn->id, props->hdr_output_metadata, state->hdr_output_metadata);
	}
}

static void add_crtc_props(struct drm_atomic *atom,
		const struct nx_drm_connector_state *state) {
	struct nx_drm_crtc *crtc = state->connector->crtc;

	drm_atomic_add(atom, crtc->id, crtc->props.mode_id, state->mode_id);
	drm_atomic_add(atom, crtc->id, crtc->props.active, state->active);
	if (!state->active) {
		return;
	}
	if (crtc->props.gamma_lut != 0) {
		drm_atomic_add(atom, crtc->id, crtc->props.gamma_lut, state->gamma_lut);
	}
	if (crtc->props.vrr_enabled != 0) {
		drm_atomic_add(atom, crtc->id, crtc->props.vrr_enabled, state->vrr_enabled);
	}
}

static void add_primary_plane(struct drm_atomic *atom,
		const struct nx_drm_connector_state *state) {
	struct nx_drm_crtc *crtc = state->connector->crtc;
	struct nx_drm_plane *primary = crtc->primary;

	set_plane_props(atom, primary, state->primary_fb, crtc->id,
		&state->primary_viewport);
	if (primary->props.fb_damage_clips != 0) {
		drm_atomic_add(atom, primary->id,
			primary->props.fb_damage_clips, state->fb_damage_clips);
	}
	if (state->primary_in_fence_fd < 0) {
		return;
	}
	if (!primary->props.in_fence_fd) {
		nx_log(NX_ERROR, "Plane %"PRIu32 " is missing the IN_FENCE_FD property",
			primary->id);
		atom->failed = true;
		return;
	}
	drm_atomic_add(atom, primary->id, primary->props.in_fence_fd,
		state->primary_in_fence_fd);
}

static void add_cursor_plane(struct drm_atomic *atom,
		const struct nx_drm_connector_state *state, bool modeset) {
	struct nx_drm_connector *conn = state->connector;
	struct nx_drm_crtc *crtc = conn->crtc;
	struct nx_drm_plane *cursor = crtc->cursor;
	if (cursor == NULL) {
		return;
	}

	if (!drm_connector_is_cursor_visible(conn)) {
		// Redundant disables race page-flips
		if (modeset || crtc->cursor_plane_on) {
			plane_disable(atom, cursor);
		}
		return;
	}

	int width = state->cursor_fb->nx_buf->width;
	int height = state->cursor_fb->nx_buf->height;
	const struct nx_drm_viewport viewport = {
		.src_box = { .width = width, .height = height },
		.dst_box = {
			.x = conn->cursor_x,
			.y = conn->cursor_y,
			.width = width,
			.height = height,
		},
	};
	set_plane_props(atom, cursor, state->cursor_fb, crtc->id, &viewport);
	if (cursor->props.hotspot_x && cursor->props.hotspot_y) {
		drm_atomic_add(atom, cursor->id, cursor->props.hotspot_x, conn->cursor_hotspot_x);
		drm_atomic_add(atom, cursor->id, cursor->props.hotspot_y, conn->cursor_hotspot_y);
	}
}

void drm_atomic_connector_add(struct drm_atomic *atom,
		struct nx_drm_connector_state *state, bool modeset) {
	add_connector_props(atom, state, modeset);
	add_crtc_props(atom, state);

	struct nx_drm_crtc *crtc = state->connector->crtc;
	if (state->active) {
		add_primary_plane(atom, state);
		add_cursor_plane(atom, state, modeset);
		return;
	}

	plane_disable(atom, crtc->primary);
	if (crtc->cursor) {
		plane_disable(atom, crtc->cursor);
	}
}
