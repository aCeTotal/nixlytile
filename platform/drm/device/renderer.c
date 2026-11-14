#include <assert.h>
#include <drm_fourcc.h>
#include <inttypes.h>
#include "platform/drm/device/device.h"
#include "platform/drm/device/renderer.h"
#include "platform/log/log.h"
#include "platform/render/allocator/allocator.h"
#include "platform/render/drm_format_set.h"
#include "platform/render/drm_syncobj.h"
#include "platform/render/pixel_format.h"
#include "platform/render/renderer.h"
#include "platform/render/swapchain.h"

bool drm_renderer_init(struct nx_drm_backend *drm,
		struct nx_drm_renderer *renderer) {
	nx_log(NX_DEBUG, "Creating multi-GPU renderer");
	renderer->nx_rend = renderer_autocreate_with_drm_fd(drm->fd);
	if (!renderer->nx_rend) {
		return false;
	}
	if (nx_renderer_get_texture_formats(renderer->nx_rend, NX_BUFFER_CAP_DMABUF) == NULL) {
		nx_log(NX_ERROR, "Renderer did not support importing DMA-BUFs");
		goto error;
	}

	renderer->allocator = nx_allocator_autocreate(&drm->backend, renderer->nx_rend);
	if (renderer->allocator == NULL) {
		goto error;
	}

	return true;

error:
	nx_renderer_destroy(renderer->nx_rend);
	renderer->nx_rend = NULL;
	return false;
}

void drm_renderer_finish(struct nx_drm_renderer *renderer) {
	nx_allocator_destroy(renderer->allocator);
	nx_renderer_destroy(renderer->nx_rend);
}

void drm_surface_finish(struct nx_drm_surface *surf) {
	if (!surf->renderer) {
		return;
	}

	nx_drm_syncobj_timeline_unref(surf->timeline);
	nx_swapchain_destroy(surf->swapchain);

	*surf = (struct nx_drm_surface){0};
}

static bool drm_surface_init(struct nx_drm_surface *surf,
		struct nx_drm_renderer *renderer, int width, int height,
		const struct nx_drm_format *drm_format) {
	if (surf->swapchain != NULL && surf->swapchain->width == width &&
			surf->swapchain->height == height) {
		return true;
	}

	drm_surface_finish(surf);

	surf->swapchain = nx_swapchain_create(renderer->allocator, width, height,
			drm_format);
	if (surf->swapchain == NULL) {
		nx_log(NX_ERROR, "Failed to create swapchain");
		return false;
	}

	int drm_fd = nx_renderer_get_drm_fd(renderer->nx_rend);
	if (renderer->nx_rend->features.timeline && drm_fd >= 0) {
		surf->timeline = nx_drm_syncobj_timeline_create(drm_fd);
		if (surf->timeline == NULL) {
			drm_surface_finish(surf);
			nx_log(NX_ERROR, "Failed to create DRM syncobj timeline");
			return false;
		}
	}

	surf->renderer = renderer;

	return true;
}

struct nx_buffer *drm_surface_blit(struct nx_drm_surface *surf,
		struct nx_buffer *buffer,
		struct nx_drm_syncobj_timeline *wait_timeline, uint64_t wait_point) {
	struct nx_renderer *renderer = surf->renderer->nx_rend;

	if (surf->swapchain->width != buffer->width ||
			surf->swapchain->height != buffer->height) {
		nx_log(NX_ERROR, "Surface size doesn't match buffer size");
		return NULL;
	}

	struct nx_texture *tex = nx_texture_from_buffer(renderer, buffer);
	if (tex == NULL) {
		nx_log(NX_ERROR, "Failed to import source buffer into multi-GPU renderer");
		return NULL;
	}

	struct nx_buffer *dst = nx_swapchain_acquire(surf->swapchain);
	if (!dst) {
		nx_log(NX_ERROR, "Failed to acquire multi-GPU swapchain buffer");
		goto error_tex;
	}

	surf->point++;
	const struct nx_buffer_pass_options pass_options = {
		.signal_timeline = surf->timeline,
		.signal_point = surf->point,
	};
	struct nx_render_pass *pass = nx_renderer_begin_buffer_pass(renderer, dst, &pass_options);
	if (pass == NULL) {
		nx_log(NX_ERROR, "Failed to begin render pass with multi-GPU destination buffer");
		goto error_dst;
	}

	nx_render_pass_add_texture(pass, &(struct nx_render_texture_options){
		.texture = tex,
		.blend_mode = NX_RENDER_BLEND_MODE_NONE,
		.wait_timeline = wait_timeline,
		.wait_point = wait_point,
	});
	if (!nx_render_pass_submit(pass)) {
		nx_log(NX_ERROR, "Failed to submit multi-GPU render pass");
		goto error_dst;
	}

	nx_texture_destroy(tex);

	return dst;

error_dst:
	nx_buffer_unlock(dst);
error_tex:
	nx_texture_destroy(tex);
	return NULL;
}

static bool drm_plane_pick_render_format(struct nx_drm_plane *plane,
		struct nx_drm_format *fmt, struct nx_drm_renderer *renderer) {
	const struct nx_drm_format_set *render_formats =
		nx_renderer_get_render_formats(renderer->nx_rend);
	if (render_formats == NULL) {
		nx_log(NX_ERROR, "Failed to get render formats");
		return false;
	}

	uint32_t format = DRM_FORMAT_ARGB8888;
	if (!nx_drm_format_set_get(&plane->formats, format)) {
		const struct nx_pixel_format_info *format_info =
			drm_get_pixel_format_info(format);
		assert(format_info != NULL &&
			format_info->opaque_substitute != DRM_FORMAT_INVALID);
		format = format_info->opaque_substitute;
	}

	const struct nx_drm_format *render_format =
		nx_drm_format_set_get(render_formats, format);
	if (render_format == NULL) {
		nx_log(NX_DEBUG, "Renderer doesn't support format 0x%"PRIX32, format);
		return false;
	}

	const struct nx_drm_format *plane_format =
		nx_drm_format_set_get(&plane->formats, format);
	if (plane_format == NULL) {
		nx_log(NX_DEBUG, "Plane %"PRIu32" doesn't support format 0x%"PRIX32,
			plane->id, format);
		return false;
	}

	if (!nx_drm_format_intersect(fmt, plane_format, render_format)) {
		nx_log(NX_DEBUG, "Failed to intersect plane and render "
			"modifiers for format 0x%"PRIX32, format);
		return false;
	}

	if (fmt->len == 0) {
		nx_drm_format_finish(fmt);
		nx_log(NX_DEBUG, "Failed to find matching plane and renderer modifiers");
		return false;
	}

	return true;
}

bool drm_plane_init_surface(struct nx_drm_plane *plane,
		struct nx_drm_renderer *renderer, const struct nx_buffer *buffer) {
	struct nx_drm_format format = {0};
	if (!drm_plane_pick_render_format(plane, &format, renderer)) {
		nx_log(NX_ERROR, "Failed to pick format for plane %"PRIu32, plane->id);
		return false;
	}

	bool ok = drm_surface_init(&plane->mgpu_surf, renderer,
		buffer->width, buffer->height, &format);
	nx_drm_format_finish(&format);
	return ok;
}
