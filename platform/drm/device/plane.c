#include <assert.h>
#include <drm_fourcc.h>
#include <drm_mode.h>
#include <inttypes.h>
#include <stdlib.h>
#include <xf86drmMode.h>
#include "platform/drm/device/device.h"
#include "platform/drm/device/fb.h"
#include "platform/log/log.h"
#include "platform/output/output_interface.h"
#include "platform/render/drm_syncobj.h"

static bool set_cursor_sizes(struct nx_drm_plane *plane,
		const struct drm_plane_size_hint *hints, size_t hints_len) {
	assert(hints_len > 0);
	plane->cursor_sizes = calloc(hints_len, sizeof(plane->cursor_sizes[0]));
	if (plane->cursor_sizes == NULL) {
		return false;
	}
	plane->cursor_sizes_len = hints_len;

	for (size_t i = 0; i < hints_len; i++) {
		plane->cursor_sizes[i] = (struct nx_output_cursor_size){
			.width = hints[i].width,
			.height = hints[i].height,
		};
	}

	return true;
}

/* SIZE_HINTS, else device cursor size. */
static bool init_cursor_sizes(struct nx_drm_backend *drm, struct nx_drm_plane *p) {
	uint64_t blob_id = 0;
	if (p->props.size_hints &&
			!drm_get_prop(drm->fd, p->id, p->props.size_hints, &blob_id)) {
		nx_log(NX_ERROR, "Failed to read SIZE_HINTS property");
		return false;
	}

	if (blob_id == 0) {
		const struct drm_plane_size_hint hint = {
			.width = drm->cursor_width,
			.height = drm->cursor_height,
		};
		return set_cursor_sizes(p, &hint, 1);
	}

	drmModePropertyBlobRes *blob = drmModeGetPropertyBlob(drm->fd, blob_id);
	if (!blob) {
		nx_log(NX_ERROR, "Failed to read SIZE_HINTS blob");
		return false;
	}

	const struct drm_plane_size_hint *hints = blob->data;
	bool ok = set_cursor_sizes(p, hints, blob->length / sizeof(hints[0]));
	drmModeFreePropertyBlob(blob);
	return ok;
}

static bool add_in_formats(struct nx_drm_backend *drm, struct nx_drm_plane *p) {
	uint64_t blob_id;
	if (!drm_get_prop(drm->fd, p->id, p->props.in_formats, &blob_id)) {
		nx_log(NX_ERROR, "Failed to read IN_FORMATS property");
		return false;
	}

	drmModePropertyBlobRes *blob = drmModeGetPropertyBlob(drm->fd, blob_id);
	if (!blob) {
		nx_log(NX_ERROR, "Failed to read IN_FORMATS blob");
		return false;
	}

	drmModeFormatModifierIterator iter = {0};
	while (drmModeFormatModifierBlobIterNext(blob, &iter)) {
		nx_drm_format_set_add(&p->formats, iter.fmt, iter.mod);
	}

	drmModeFreePropertyBlob(blob);
	return true;
}

static bool init_plane(struct nx_drm_backend *drm,
		struct nx_drm_plane *p, const drmModePlane *drm_plane) {
	uint32_t id = drm_plane->plane_id;

	struct nx_drm_plane_props props = {0};
	if (!drm_get_plane_props(drm->fd, id, &props)) {
		return false;
	}

	uint64_t type;
	if (!drm_get_prop(drm->fd, id, props.type, &type)) {
		return false;
	}

	p->id = id;
	p->props = props;

	for (size_t i = 0; i < drm_plane->count_formats; ++i) {
		nx_drm_format_set_add(&p->formats, drm_plane->formats[i],
			DRM_FORMAT_MOD_LINEAR);
		nx_drm_format_set_add(&p->formats, drm_plane->formats[i],
			DRM_FORMAT_MOD_INVALID);
	}

	if (p->props.in_formats && drm->addfb2_modifiers && !add_in_formats(drm, p)) {
		return false;
	}
	if (!init_cursor_sizes(drm, p)) {
		return false;
	}

	// Fill free primary/cursor slots
	assert(drm->num_crtcs <= 32);
	for (size_t j = 0; j < drm->num_crtcs; j++) {
		struct nx_drm_crtc *crtc = &drm->crtcs[j];
		if ((drm_plane->possible_crtcs & (1 << j)) == 0) {
			continue;
		}
		if (type == DRM_PLANE_TYPE_PRIMARY && !crtc->primary) {
			crtc->primary = p;
			break;
		}
		if (type == DRM_PLANE_TYPE_CURSOR && !crtc->cursor) {
			crtc->cursor = p;
			break;
		}
	}

	return true;
}

bool drm_init_planes(struct nx_drm_backend *drm) {
	drmModePlaneRes *plane_res = drmModeGetPlaneResources(drm->fd);
	if (!plane_res) {
		nx_log_errno(NX_ERROR, "Failed to get DRM plane resources");
		return false;
	}

	nx_log(NX_INFO, "Found %"PRIu32" DRM planes", plane_res->count_planes);

	drm->num_planes = plane_res->count_planes;
	drm->planes = calloc(drm->num_planes, sizeof(*drm->planes));
	if (drm->planes == NULL) {
		nx_log_errno(NX_ERROR, "Allocation failed");
		goto error;
	}

	for (uint32_t i = 0; i < plane_res->count_planes; ++i) {
		drmModePlane *drm_plane = drmModeGetPlane(drm->fd, plane_res->planes[i]);
		if (!drm_plane) {
			nx_log_errno(NX_ERROR, "Failed to get DRM plane");
			goto error;
		}

		bool ok = init_plane(drm, &drm->planes[i], drm_plane);
		drmModeFreePlane(drm_plane);
		if (!ok) {
			goto error;
		}
	}

	drmModeFreePlaneResources(plane_res);
	return true;

error:
	free(drm->planes);
	drmModeFreePlaneResources(plane_res);
	return false;
}

void drm_plane_release(struct nx_drm_syncobj_timeline **timeline,
		uint64_t *point) {
	if (*timeline != NULL) {
		nx_drm_syncobj_timeline_signal(*timeline, *point);
		nx_drm_syncobj_timeline_unref(*timeline);
	}
	*timeline = NULL;
	*point = 0;
}

void drm_plane_finish_surface(struct nx_drm_plane *plane) {
	if (!plane) {
		return;
	}

	drm_fb_clear(&plane->queued_fb);
	drm_plane_release(&plane->queued_release_timeline, &plane->queued_release_point);
	drm_fb_clear(&plane->current_fb);
	drm_plane_release(&plane->current_release_timeline, &plane->current_release_point);

	drm_surface_finish(&plane->mgpu_surf);
}
