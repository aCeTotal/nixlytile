#include <stdlib.h>
#include <xf86drm.h>
#include "platform/drm/commit/commit.h"
#include "platform/drm/device/fb.h"
#include "platform/log/log.h"
#include "platform/output/output_interface.h"
#include "platform/session/session.h"

struct nx_drm_page_flip *drm_page_flip_create(struct nx_drm_backend *drm,
		const struct nx_drm_device_state *state) {
	struct nx_drm_page_flip *page_flip = calloc(1, sizeof(*page_flip));
	if (page_flip == NULL) {
		return NULL;
	}
	page_flip->connectors_len = state->connectors_len;
	page_flip->connectors =
		calloc(page_flip->connectors_len, sizeof(page_flip->connectors[0]));
	if (page_flip->connectors == NULL) {
		free(page_flip);
		return NULL;
	}
	for (size_t i = 0; i < state->connectors_len; i++) {
		struct nx_drm_connector *conn = state->connectors[i].connector;
		page_flip->connectors[i] = (struct nx_drm_page_flip_connector){
			.connector = conn,
			.crtc_id = conn->crtc->id,
		};
	}
	wl_list_insert(&drm->page_flips, &page_flip->link);
	return page_flip;
}

void drm_page_flip_destroy(struct nx_drm_page_flip *page_flip) {
	if (!page_flip) {
		return;
	}

	wl_list_remove(&page_flip->link);
	free(page_flip->connectors);
	free(page_flip);
}

struct nx_drm_connector *drm_page_flip_pop(struct nx_drm_page_flip *page_flip,
		uint32_t crtc_id) {
	for (size_t i = 0; i < page_flip->connectors_len; i++) {
		if (page_flip->connectors[i].crtc_id != crtc_id) {
			continue;
		}
		struct nx_drm_connector *conn = page_flip->connectors[i].connector;
		page_flip->connectors_len--;
		page_flip->connectors[i] = page_flip->connectors[page_flip->connectors_len];
		return conn;
	}
	return NULL;
}

void drm_connector_set_pending_page_flip(struct nx_drm_connector *conn,
		struct nx_drm_page_flip *page_flip) {
	struct nx_drm_page_flip *old = conn->pending_page_flip;
	conn->pending_page_flip = page_flip;
	if (old == NULL) {
		return;
	}

	drm_conn_log(conn, NX_ERROR,
		"pending page-flip %s while flip event still outstanding "
		"(old event will be orphaned)",
		page_flip != NULL ? "replaced" : "cleared");
	for (size_t i = 0; i < old->connectors_len; i++) {
		if (old->connectors[i].connector == conn) {
			old->connectors[i].connector = NULL;
		}
	}
}

static int mhz_to_nsec(int mhz) {
	return 1000000000000LL / mhz;
}

static void promote_queued_primary(struct nx_drm_plane *plane) {
	if (!plane->queued_fb) {
		return;
	}
	drm_fb_move(&plane->current_fb, &plane->queued_fb);
	drm_plane_release(&plane->current_release_timeline, &plane->current_release_point);
	plane->current_release_timeline = plane->queued_release_timeline;
	plane->current_release_point = plane->queued_release_point;
	plane->queued_release_timeline = NULL;
	plane->queued_release_point = 0;
}

static void handle_page_flip(int fd, unsigned seq,
		unsigned tv_sec, unsigned tv_usec, unsigned crtc_id, void *data) {
	struct nx_drm_page_flip *page_flip = data;

	struct nx_drm_connector *conn = drm_page_flip_pop(page_flip, crtc_id);
	if (conn != NULL) {
		conn->pending_page_flip = NULL;
	}

	uint32_t present_flags = NX_OUTPUT_PRESENT_HW_CLOCK | NX_OUTPUT_PRESENT_HW_COMPLETION;
	if (!page_flip->async) {
		present_flags |= NX_OUTPUT_PRESENT_VSYNC;
	}

	if (page_flip->connectors_len == 0) {
		drm_page_flip_destroy(page_flip);
	}

	if (conn == NULL) {
		nx_log(NX_ERROR, "page-flip event for CRTC %u matched no connector "
			"(orphaned event)", crtc_id);
		return;
	}

	struct nx_drm_backend *drm = conn->backend;

	if (conn->status != DRM_MODE_CONNECTED || conn->crtc == NULL) {
		drm_conn_log(conn, NX_DEBUG,
			"Ignoring page-flip event for disabled connector");
		return;
	}

	promote_queued_primary(conn->crtc->primary);
	if (conn->crtc->cursor && conn->crtc->cursor->queued_fb) {
		drm_fb_move(&conn->crtc->cursor->current_fb,
			&conn->crtc->cursor->queued_fb);
	}

	// Secondary GPUs copied the frame
	if (!drm->mgpu_renderer.nx_rend) {
		present_flags |= NX_OUTPUT_PRESENT_ZERO_COPY;
	}

	struct nx_output_event_present present_event = {
		// Always the last submitted frame
		.commit_seq = conn->output.commit_seq,
		.presented = drm->session->active,
		.when = {
			.tv_sec = tv_sec,
			.tv_nsec = tv_usec * 1000,
		},
		.seq = seq,
		.refresh = mhz_to_nsec(conn->refresh),
		.flags = present_flags,
	};
	nx_output_send_present(&conn->output, &present_event);

	if (drm->session->active) {
		nx_output_send_frame(&conn->output);
	}
}

int drm_handle_event(int fd, uint32_t mask, void *data) {
	struct nx_drm_backend *drm = data;

	drmEventContext event = {
		.version = 3,
		.page_flip_handler2 = handle_page_flip,
	};

	if (drmHandleEvent(fd, &event) != 0) {
		nx_log(NX_ERROR, "drmHandleEvent failed");
		nx_backend_destroy(&drm->backend);
	}
	return 1;
}
