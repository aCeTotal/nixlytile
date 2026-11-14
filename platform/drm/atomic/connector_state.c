#include <poll.h>
#include <stdlib.h>
#include <unistd.h>
#include "platform/drm/atomic/atomic.h"
#include "platform/drm/device/fb.h"
#include "platform/log/log.h"
#include "platform/render/drm_syncobj.h"

#define FENCE_WAIT_TIMEOUT_MS 100
#define COLORSPACE_DEFAULT 0
#define COLORSPACE_BT2020_RGB 9
#define CONTENT_TYPE_PHOTO 1
#define CONTENT_TYPE_VIDEO 2
#define CONTENT_TYPE_GAME 3

void drm_atomic_wait_fence(int fd) {
	struct pollfd pfd = { .fd = fd, .events = POLLIN };
	poll(&pfd, 1, FENCE_WAIT_TIMEOUT_MS);
	close(fd);
}

static uint64_t primaries_to_colorspace(uint32_t primaries) {
	switch (primaries) {
	case 0:
		return COLORSPACE_DEFAULT;
	case NX_COLOR_NAMED_PRIMARIES_BT2020:
		return COLORSPACE_BT2020_RGB;
	}
	abort();
}

/* Maps wp_content_type_v1 values. */
static uint32_t drm_content_type(const struct nx_output_state *base) {
	if (!(base->committed & NX_OUTPUT_STATE_CONTENT_TYPE)) {
		return DRM_MODE_CONTENT_TYPE_GRAPHICS;
	}
	switch (base->content_type) {
	case CONTENT_TYPE_PHOTO:
		return DRM_MODE_CONTENT_TYPE_PHOTO;
	case CONTENT_TYPE_VIDEO:
		return DRM_MODE_CONTENT_TYPE_CINEMA;
	case CONTENT_TYPE_GAME:
		return DRM_MODE_CONTENT_TYPE_GAME;
	default:
		return DRM_MODE_CONTENT_TYPE_GRAPHICS;
	}
}

/* NVIDIA 555+ rejects IN_FENCE_FD. */
static int usable_in_fence(struct nx_drm_backend *drm, int fd) {
	if (!drm->in_fence_fd_broken) {
		return fd;
	}
	drm_atomic_wait_fence(fd);
	return -1;
}

bool drm_atomic_connector_prepare(struct nx_drm_connector_state *state, bool modeset) {
	struct nx_drm_connector *conn = state->connector;
	struct nx_drm_backend *drm = conn->backend;
	struct nx_output *output = &conn->output;
	struct nx_drm_crtc *crtc = conn->crtc;
	const struct nx_output_state *base = state->base;

	uint32_t mode_id = crtc->mode_id;
	if (modeset && !drm_atomic_mode_blob(state, &mode_id)) {
		return false;
	}

	// Older Intel lacks GAMMA_LUT
	uint32_t gamma_lut = crtc->gamma_lut;
	bool set_gamma = base->committed & NX_OUTPUT_STATE_COLOR_TRANSFORM;
	if (set_gamma && crtc->props.gamma_lut == 0 &&
			!drm_legacy_crtc_set_gamma(drm, crtc, base->color_transform)) {
		return false;
	}
	if (set_gamma && crtc->props.gamma_lut != 0 &&
			!drm_atomic_gamma_blob(drm, base->color_transform, &gamma_lut)) {
		return false;
	}

	uint32_t fb_damage_clips = 0;
	if ((base->committed & NX_OUTPUT_STATE_DAMAGE) &&
			crtc->primary->props.fb_damage_clips != 0) {
		fb_damage_clips = drm_atomic_damage_blob(drm,
			state->primary_fb->nx_buf, &base->damage);
	}

	int in_fence_fd = -1;
	if (state->wait_timeline != NULL) {
		in_fence_fd = nx_drm_syncobj_timeline_export_sync_file(state->wait_timeline,
			state->wait_point);
		if (in_fence_fd < 0) {
			return false;
		}
		in_fence_fd = usable_in_fence(drm, in_fence_fd);
	}

	bool vrr_committed = base->committed & NX_OUTPUT_STATE_ADAPTIVE_SYNC_ENABLED;
	if (vrr_committed && base->adaptive_sync_enabled && !output->adaptive_sync_supported) {
		return false;
	}

	uint32_t colorspace = conn->colorspace;
	uint32_t hdr_output_metadata = conn->hdr_output_metadata;
	if (base->committed & NX_OUTPUT_STATE_IMAGE_DESCRIPTION) {
		colorspace = primaries_to_colorspace(
			base->image_description ? base->image_description->primaries : 0);
		if (!drm_atomic_hdr_metadata_blob(drm, base->image_description, &hdr_output_metadata)) {
			return false;
		}
	}

	state->mode_id = mode_id;
	state->gamma_lut = gamma_lut;
	state->fb_damage_clips = fb_damage_clips;
	state->primary_in_fence_fd = in_fence_fd;
	state->vrr_enabled = vrr_committed ? base->adaptive_sync_enabled :
		output->adaptive_sync_status == NX_OUTPUT_ADAPTIVE_SYNC_ENABLED;
	state->colorspace = colorspace;
	state->hdr_output_metadata = hdr_output_metadata;
	state->content_type = modeset || !conn->content_type_needs_modeset ?
		drm_content_type(base) : conn->content_type;
	state->content_type_deferred = false;
	return true;
}

static void destroy_blob(struct nx_drm_backend *drm, uint32_t id) {
	if (id == 0) {
		return;
	}
	if (drmModeDestroyPropertyBlob(drm->fd, id) != 0) {
		nx_log_errno(NX_ERROR, "Failed to destroy blob");
	}
}

static void commit_blob(struct nx_drm_backend *drm,
		uint32_t *current, uint32_t next) {
	if (*current == next) {
		return;
	}
	destroy_blob(drm, *current);
	*current = next;
}

static void rollback_blob(struct nx_drm_backend *drm,
		uint32_t *current, uint32_t next) {
	if (*current == next) {
		return;
	}
	destroy_blob(drm, next);
}

void drm_atomic_connector_apply_commit(struct nx_drm_connector_state *state) {
	struct nx_drm_connector *conn = state->connector;
	struct nx_drm_crtc *crtc = conn->crtc;
	struct nx_drm_backend *drm = conn->backend;

	if (!crtc->own_mode_id) {
		crtc->mode_id = 0; // previous master's blobs aren't ours
	}
	crtc->own_mode_id = true;
	commit_blob(drm, &crtc->mode_id, state->mode_id);
	commit_blob(drm, &crtc->gamma_lut, state->gamma_lut);
	commit_blob(drm, &conn->hdr_output_metadata, state->hdr_output_metadata);

	conn->output.adaptive_sync_status = state->vrr_enabled ?
		NX_OUTPUT_ADAPTIVE_SYNC_ENABLED : NX_OUTPUT_ADAPTIVE_SYNC_DISABLED;

	destroy_blob(drm, state->fb_damage_clips);
	if (state->primary_in_fence_fd >= 0) {
		close(state->primary_in_fence_fd);
	}

	conn->colorspace = state->colorspace;
	conn->content_type = state->content_type;
	crtc->cursor_plane_on = state->active && drm_connector_is_cursor_visible(conn);
}

void drm_atomic_connector_rollback_commit(struct nx_drm_connector_state *state) {
	struct nx_drm_connector *conn = state->connector;
	struct nx_drm_crtc *crtc = conn->crtc;
	struct nx_drm_backend *drm = conn->backend;

	rollback_blob(drm, &crtc->mode_id, state->mode_id);
	rollback_blob(drm, &crtc->gamma_lut, state->gamma_lut);
	rollback_blob(drm, &conn->hdr_output_metadata, state->hdr_output_metadata);

	destroy_blob(drm, state->fb_damage_clips);
	if (state->primary_in_fence_fd >= 0) {
		close(state->primary_in_fence_fd);
	}
}
