#include "platform/buffer/buffer.h"
#include "platform/drm/connector/connector.h"
#include "platform/drm/device/fb.h"
#include "platform/geometry/box.h"
#include "platform/geometry/transform.h"
#include "platform/output/output_interface.h"
#include "platform/render/drm_format_set.h"

static bool cursor_size_supported(const struct nx_drm_plane *plane,
		const struct nx_buffer *buffer) {
	for (size_t i = 0; i < plane->cursor_sizes_len; i++) {
		const struct nx_output_cursor_size *size = &plane->cursor_sizes[i];
		if (size->width == buffer->width && size->height == buffer->height) {
			return true;
		}
	}
	return false;
}

static struct nx_buffer *lock_scanout_buffer(struct nx_drm_backend *drm,
		struct nx_drm_plane *plane, struct nx_buffer *buffer) {
	if (!drm->mgpu_renderer.nx_rend) {
		return nx_buffer_lock(buffer);
	}

	if (!drm_plane_init_surface(plane, &drm->mgpu_renderer, buffer)) {
		return NULL;
	}

	return drm_surface_blit(&plane->mgpu_surf, buffer, NULL, 0);
}

bool drm_connector_set_cursor(struct nx_output *output,
		struct nx_buffer *buffer, int hotspot_x, int hotspot_y) {
	struct nx_drm_connector *conn = drm_connector_from_output(output);
	struct nx_drm_backend *drm = conn->backend;
	if (!conn->crtc || conn->crtc->cursor == NULL) {
		return false;
	}
	struct nx_drm_plane *plane = conn->crtc->cursor;

	// Keep the tip in place
	conn->cursor_x -= hotspot_x - conn->cursor_hotspot_x;
	conn->cursor_y -= hotspot_y - conn->cursor_hotspot_y;
	conn->cursor_hotspot_x = hotspot_x;
	conn->cursor_hotspot_y = hotspot_y;

	conn->cursor_enabled = false;
	drm_fb_clear(&conn->cursor_pending_fb);
	if (buffer == NULL) {
		return true;
	}

	if (!cursor_size_supported(plane, buffer)) {
		drm_conn_log(conn, NX_DEBUG, "Cursor buffer size mismatch");
		return false;
	}

	struct nx_buffer *local_buf = lock_scanout_buffer(drm, plane, buffer);
	if (local_buf == NULL) {
		return false;
	}

	bool ok = drm_fb_import(&conn->cursor_pending_fb, drm, local_buf,
		&plane->formats);
	nx_buffer_unlock(local_buf);
	if (!ok) {
		return false;
	}

	conn->cursor_enabled = true;
	conn->cursor_width = buffer->width;
	conn->cursor_height = buffer->height;
	return true;
}

bool drm_connector_move_cursor(struct nx_output *output, int x, int y) {
	struct nx_drm_connector *conn = drm_connector_from_output(output);
	if (!conn->crtc || !conn->crtc->cursor) {
		return false;
	}

	struct nx_box box = { .x = x, .y = y };

	int width, height;
	nx_output_transformed_resolution(output, &width, &height);

	enum wl_output_transform transform =
		nx_output_transform_invert(output->transform);
	nx_box_transform(&box, &box, transform, width, height);

	conn->cursor_x = box.x - conn->cursor_hotspot_x;
	conn->cursor_y = box.y - conn->cursor_hotspot_y;
	return true;
}

bool drm_connector_is_cursor_visible(struct nx_drm_connector *conn) {
	return conn->cursor_enabled &&
		conn->cursor_x < conn->output.width &&
		conn->cursor_y < conn->output.height &&
		conn->cursor_x + conn->cursor_width >= 0 &&
		conn->cursor_y + conn->cursor_height >= 0;
}

const struct nx_drm_format_set *drm_connector_get_cursor_formats(
		struct nx_output *output, uint32_t buffer_caps) {
	if (!(buffer_caps & NX_BUFFER_CAP_DMABUF)) {
		return NULL;
	}
	struct nx_drm_connector *conn = drm_connector_from_output(output);
	if (!drm_connector_alloc_crtc(conn) || !conn->crtc->cursor) {
		return NULL;
	}
	if (conn->backend->mgpu_renderer.nx_rend) {
		return &conn->backend->mgpu_formats;
	}
	return &conn->crtc->cursor->formats;
}

const struct nx_output_cursor_size *drm_connector_get_cursor_sizes(
		struct nx_output *output, size_t *len) {
	struct nx_drm_connector *conn = drm_connector_from_output(output);
	if (!drm_connector_alloc_crtc(conn) || !conn->crtc->cursor) {
		return NULL;
	}
	*len = conn->crtc->cursor->cursor_sizes_len;
	return conn->crtc->cursor->cursor_sizes;
}
