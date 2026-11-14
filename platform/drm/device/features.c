#include <xf86drm.h>
#include "platform/drm/commit/commit.h"
#include "platform/drm/device/device.h"
#include "platform/log/log.h"
#include "platform/os/env.h"

#define DEFAULT_CURSOR_SIZE 64

/* Zero when the query fails. */
static uint64_t get_cap(int fd, uint64_t capability) {
	uint64_t value;
	if (drmGetCap(fd, capability, &value) != 0) {
		return 0;
	}
	return value;
}

static const struct nx_drm_interface *pick_interface(int fd) {
	if (env_parse_bool("NIXLY_DRM_NO_ATOMIC")) {
		nx_log(NX_DEBUG,
			"NIXLY_DRM_NO_ATOMIC set, forcing legacy DRM interface");
		return &drm_legacy_iface;
	}
	if (drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1)) {
		nx_log(NX_DEBUG,
			"Atomic modesetting unsupported, using legacy DRM interface");
		return &drm_legacy_iface;
	}
	nx_log(NX_DEBUG, "Using atomic DRM interface");
	return &drm_atomic_iface;
}

static bool has_required_caps(struct nx_drm_backend *drm) {
	if (!(get_cap(drm->fd, DRM_CAP_PRIME) & DRM_PRIME_CAP_IMPORT)) {
		nx_log(NX_ERROR, "PRIME import not supported");
		return false;
	}

	if (drm->parent && !(get_cap(drm->parent->fd, DRM_CAP_PRIME) & DRM_PRIME_CAP_EXPORT)) {
		nx_log(NX_ERROR, "PRIME export not supported on primary GPU");
		return false;
	}

	if (drmSetClientCap(drm->fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1)) {
		nx_log(NX_ERROR, "DRM universal planes unsupported");
		return false;
	}

	if (!get_cap(drm->fd, DRM_CAP_CRTC_IN_VBLANK_EVENT)) {
		nx_log(NX_ERROR, "DRM_CRTC_IN_VBLANK_EVENT unsupported");
		return false;
	}

	if (!get_cap(drm->fd, DRM_CAP_TIMESTAMP_MONOTONIC)) {
		nx_log(NX_ERROR, "DRM_CAP_TIMESTAMP_MONOTONIC unsupported");
		return false;
	}

	return true;
}

bool drm_check_features(struct nx_drm_backend *drm) {
	if (drmGetCap(drm->fd, DRM_CAP_CURSOR_WIDTH, &drm->cursor_width)) {
		drm->cursor_width = DEFAULT_CURSOR_SIZE;
	}
	if (drmGetCap(drm->fd, DRM_CAP_CURSOR_HEIGHT, &drm->cursor_height)) {
		drm->cursor_height = DEFAULT_CURSOR_SIZE;
	}

	if (!has_required_caps(drm)) {
		return false;
	}

	drm->iface = pick_interface(drm->fd);
	if (drm->iface == &drm_atomic_iface &&
			drmSetClientCap(drm->fd, DRM_CLIENT_CAP_CURSOR_PLANE_HOTSPOT, 1) == 0) {
		nx_log(NX_INFO, "DRM_CLIENT_CAP_CURSOR_PLANE_HOTSPOT supported");
	}

	if (drm->iface == &drm_legacy_iface) {
		drm->supports_tearing_page_flips = get_cap(drm->fd, DRM_CAP_ASYNC_PAGE_FLIP) == 1;
	} else {
		drm->supports_tearing_page_flips = get_cap(drm->fd, DRM_CAP_ATOMIC_ASYNC_PAGE_FLIP) == 1;
		drm->backend.features.timeline = get_cap(drm->fd, DRM_CAP_SYNCOBJ_TIMELINE) == 1;
	}

	// NVIDIA needs modifiers despite NO_MODIFIERS
	drm->cap_addfb2_modifiers = get_cap(drm->fd, DRM_CAP_ADDFB2_MODIFIERS) == 1;
	if (env_parse_bool("NIXLY_DRM_NO_MODIFIERS")) {
		nx_log(NX_DEBUG, "NIXLY_DRM_NO_MODIFIERS set, disabling modifiers");
		return true;
	}

	drm->addfb2_modifiers = drm->cap_addfb2_modifiers;
	nx_log(NX_DEBUG, "ADDFB2 modifiers %s",
		drm->addfb2_modifiers ? "supported" : "unsupported");
	return true;
}
