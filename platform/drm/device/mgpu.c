#include <drm_fourcc.h>
#include "platform/drm/device/device.h"
#include "platform/drm/device/renderer.h"
#include "platform/log/log.h"
#include "platform/render/drm_format_set.h"
#include "platform/render/renderer.h"

/* Implicit modifiers don't cross devices. */
static void strip_implicit_modifiers(struct nx_drm_format_set *set) {
	for (size_t i = 0; i < set->len; i++) {
		nx_drm_format_set_remove(set, set->formats[i].format, DRM_FORMAT_MOD_INVALID);
	}
}

static bool has_implicit_modifier(const struct nx_drm_format *fmt) {
	for (size_t i = 0; i < fmt->len; i++) {
		if (fmt->modifiers[i] == DRM_FORMAT_MOD_INVALID) {
			return true;
		}
	}
	return false;
}

/* NVIDIA planes list only INVALID. */
static void add_linear_to_implicit(struct nx_drm_format_set *set) {
	for (size_t i = 0; i < set->len; i++) {
		if (has_implicit_modifier(&set->formats[i])) {
			nx_drm_format_set_add(set, set->formats[i].format, DRM_FORMAT_MOD_LINEAR);
		}
	}
}

bool drm_mgpu_init(struct nx_drm_backend *drm) {
	if (!drm_renderer_init(drm, &drm->mgpu_renderer)) {
		nx_log(NX_INFO, "Failed to initialize mgpu blit renderer"
			", falling back to scanning out from primary GPU");
		for (size_t i = 0; i < drm->num_planes; i++) {
			strip_implicit_modifiers(&drm->planes[i].formats);
		}
		return true;
	}

	struct nx_renderer *renderer = drm->mgpu_renderer.nx_rend;
	const struct nx_drm_format_set *texture_formats =
		nx_renderer_get_texture_formats(renderer, NX_BUFFER_CAP_DMABUF);
	if (texture_formats == NULL) {
		nx_log(NX_ERROR, "Failed to query renderer texture formats");
		return false;
	}

	nx_drm_format_set_copy(&drm->mgpu_formats, texture_formats);
	for (size_t i = 0; i < drm->mgpu_formats.len; i++) {
		nx_drm_format_set_add(&drm->mgpu_formats,
			drm->mgpu_formats.formats[i].format, DRM_FORMAT_MOD_LINEAR);
	}
	strip_implicit_modifiers(&drm->mgpu_formats);

	for (size_t i = 0; i < drm->num_planes; i++) {
		add_linear_to_implicit(&drm->planes[i].formats);
	}

	drm->backend.features.timeline = drm->backend.features.timeline &&
		renderer->features.timeline;
	return true;
}
