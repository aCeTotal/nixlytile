#include <assert.h>
#include <stdlib.h>
#include "platform/buffer/buffer.h"
#include "platform/drm/commit/commit.h"
#include "platform/drm/connector/connector.h"
#include "platform/output/output_interface.h"

struct nx_drm_connector *drm_connector_from_output(struct nx_output *output) {
	assert(nx_output_is_drm(output));
	struct nx_drm_connector *conn = wl_container_of(output, conn, output);
	return conn;
}

static bool drm_connector_test(struct nx_output *output,
		const struct nx_output_state *state) {
	return drm_connector_commit_state(drm_connector_from_output(output), state, true);
}

static bool drm_connector_commit(struct nx_output *output,
		const struct nx_output_state *state) {
	return drm_connector_commit_state(drm_connector_from_output(output), state, false);
}

static size_t drm_connector_get_gamma_size(struct nx_output *output) {
	struct nx_drm_connector *conn = drm_connector_from_output(output);
	if (conn->crtc == NULL) {
		return 0;
	}
	return conn->crtc->gamma_lut_size;
}

/* Keeps the connector for reconnects. */
static void drm_connector_destroy_output(struct nx_output *output) {
	struct nx_drm_connector *conn = drm_connector_from_output(output);

	nx_output_finish(output);

	drm_connector_dealloc_crtc(conn);

	conn->status = DRM_MODE_DISCONNECTED;
	drm_connector_set_pending_page_flip(conn, NULL);

	struct nx_drm_mode *mode, *mode_tmp;
	wl_list_for_each_safe(mode, mode_tmp, &conn->output.modes, nx_mode.link) {
		wl_list_remove(&mode->nx_mode.link);
		free(mode);
	}

	conn->output = (struct nx_output){0};
}

static const struct nx_drm_format_set *drm_connector_get_primary_formats(
		struct nx_output *output, uint32_t buffer_caps) {
	if (!(buffer_caps & NX_BUFFER_CAP_DMABUF)) {
		return NULL;
	}
	struct nx_drm_connector *conn = drm_connector_from_output(output);
	if (!drm_connector_alloc_crtc(conn)) {
		return NULL;
	}
	if (conn->backend->mgpu_renderer.nx_rend) {
		return &conn->backend->mgpu_formats;
	}
	return &conn->crtc->primary->formats;
}

const struct nx_output_impl drm_output_impl = {
	.set_cursor = drm_connector_set_cursor,
	.move_cursor = drm_connector_move_cursor,
	.destroy = drm_connector_destroy_output,
	.test = drm_connector_test,
	.commit = drm_connector_commit,
	.get_gamma_size = drm_connector_get_gamma_size,
	.get_cursor_formats = drm_connector_get_cursor_formats,
	.get_cursor_sizes = drm_connector_get_cursor_sizes,
	.get_primary_formats = drm_connector_get_primary_formats,
};

bool nx_output_is_drm(struct nx_output *output) {
	return output->impl == &drm_output_impl;
}

uint32_t nx_drm_connector_get_id(struct nx_output *output) {
	return drm_connector_from_output(output)->id;
}
