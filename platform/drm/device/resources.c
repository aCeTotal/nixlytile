#include <stdlib.h>
#include <xf86drmMode.h>
#include "platform/drm/commit/commit.h"
#include "platform/drm/device/device.h"
#include "platform/log/log.h"

static size_t read_gamma_lut_size(struct nx_drm_backend *drm,
		const struct nx_drm_crtc *crtc, int legacy_size) {
	if (crtc->props.gamma_lut_size == 0 || drm->iface == &drm_legacy_iface) {
		return (size_t)legacy_size;
	}

	uint64_t size;
	if (!drm_get_prop(drm->fd, crtc->id, crtc->props.gamma_lut_size, &size)) {
		nx_log(NX_ERROR, "Unable to get gamma lut size");
		return 0;
	}
	return size;
}

static bool init_crtc(struct nx_drm_backend *drm, struct nx_drm_crtc *crtc,
		uint32_t id) {
	crtc->id = id;

	drmModeCrtc *drm_crtc = drmModeGetCrtc(drm->fd, id);
	if (drm_crtc == NULL) {
		nx_log_errno(NX_ERROR, "drmModeGetCrtc failed");
		return false;
	}
	int legacy_size = drm_crtc->gamma_size;
	drmModeFreeCrtc(drm_crtc);

	if (!drm_get_crtc_props(drm->fd, id, &crtc->props)) {
		return false;
	}
	crtc->gamma_lut_size = read_gamma_lut_size(drm, crtc, legacy_size);
	return true;
}

static bool init_crtcs(struct nx_drm_backend *drm, const drmModeRes *res) {
	drm->crtcs = calloc(drm->num_crtcs, sizeof(drm->crtcs[0]));
	if (!drm->crtcs) {
		nx_log_errno(NX_ERROR, "Allocation failed");
		return false;
	}

	for (size_t i = 0; i < drm->num_crtcs; ++i) {
		if (!init_crtc(drm, &drm->crtcs[i], res->crtcs[i])) {
			goto error;
		}
	}

	if (!drm_init_planes(drm)) {
		goto error;
	}

	return true;

error:
	free(drm->crtcs);
	return false;
}

bool drm_init_resources(struct nx_drm_backend *drm) {
	drmModeRes *res = drmModeGetResources(drm->fd);
	if (!res) {
		nx_log_errno(NX_ERROR, "Failed to get DRM resources");
		return false;
	}

	nx_log(NX_INFO, "Found %d DRM CRTCs", res->count_crtcs);

	drm->num_crtcs = res->count_crtcs;
	bool ok = drm->num_crtcs == 0 || init_crtcs(drm, res);
	drmModeFreeResources(res);
	return ok;
}

void drm_finish_resources(struct nx_drm_backend *drm) {
	for (size_t i = 0; i < drm->num_crtcs; ++i) {
		struct nx_drm_crtc *crtc = &drm->crtcs[i];

		if (crtc->mode_id && crtc->own_mode_id) {
			drmModeDestroyPropertyBlob(drm->fd, crtc->mode_id);
		}
		if (crtc->gamma_lut) {
			drmModeDestroyPropertyBlob(drm->fd, crtc->gamma_lut);
		}
	}

	free(drm->crtcs);

	for (size_t i = 0; i < drm->num_planes; ++i) {
		struct nx_drm_plane *plane = &drm->planes[i];
		drm_plane_finish_surface(plane);
		nx_drm_format_set_finish(&plane->formats);
		free(plane->cursor_sizes);
	}

	free(drm->planes);
}
