#include <assert.h>
#include <drm_fourcc.h>
#include "platform/render/allocator/allocator.h"
#include "platform/render/swapchain.h"
#include "platform/geometry/transform.h"
#include "platform/log/log.h"
#include "platform/output/output_interface.h"
#include "platform/output/commit/commit.h"
#include "platform/output/cursor/cursor.h"

static bool output_pick_cursor_format(struct nx_output *output,
		struct nx_drm_format *format) {
	struct nx_allocator *allocator = output->allocator;
	assert(allocator != NULL);

	const struct nx_drm_format_set *display_formats = NULL;
	if (output->impl->get_cursor_formats) {
		display_formats =
			output->impl->get_cursor_formats(output, allocator->buffer_caps);
		if (display_formats == NULL) {
			nx_log(NX_DEBUG, "Failed to get cursor display formats");
			return false;
		}
	}

	return output_pick_format(output, display_formats, format, DRM_FORMAT_ARGB8888);
}

// First fitting plane size.
static bool pick_cursor_size(struct nx_output_cursor *cursor,
		int *width, int *height) {
	struct nx_output *output = cursor->output;
	*width = cursor->width;
	*height = cursor->height;
	if (!output->impl->get_cursor_sizes) {
		return true;
	}

	size_t sizes_len = 0;
	const struct nx_output_cursor_size *sizes =
		output->impl->get_cursor_sizes(output, &sizes_len);
	if (sizes_len == 0) {
		nx_log(NX_DEBUG, "Hardware cursor not supported");
		return false;
	}

	struct nx_texture *texture = cursor->texture;
	for (size_t i = 0; i < sizes_len; i++) {
		if ((int)texture->width <= sizes[i].width &&
				(int)texture->height <= sizes[i].height) {
			*width = sizes[i].width;
			*height = sizes[i].height;
			return true;
		}
	}

	nx_log(NX_DEBUG, "Cursor texture too large (%dx%d), "
		"exceeds hardware limitations", texture->width, texture->height);
	return false;
}

static bool ensure_cursor_swapchain(struct nx_output *output,
		int width, int height) {
	struct nx_swapchain *swapchain = output->cursor_swapchain;
	if (swapchain != NULL && swapchain->width == width &&
			swapchain->height == height) {
		return true;
	}
	// A format failure is permanent.
	if (output->cursor_format_failed) {
		return false;
	}

	struct nx_drm_format format = {0};
	if (!output_pick_cursor_format(output, &format)) {
		nx_log(NX_DEBUG, "Failed to pick cursor format");
		output->cursor_format_failed = true;
		return false;
	}

	nx_swapchain_destroy(output->cursor_swapchain);
	output->cursor_swapchain = nx_swapchain_create(output->allocator,
		width, height, &format);
	nx_drm_format_finish(&format);
	if (output->cursor_swapchain == NULL) {
		nx_log(NX_ERROR, "Failed to create cursor swapchain");
		return false;
	}
	return true;
}

static bool draw_cursor(struct nx_output_cursor *cursor,
		struct nx_buffer *buffer) {
	struct nx_output *output = cursor->output;
	struct nx_box dst_box = {
		.width = cursor->width,
		.height = cursor->height,
	};
	nx_box_transform(&dst_box, &dst_box, nx_output_transform_invert(output->transform),
		buffer->width, buffer->height);

	struct nx_buffer_pass_options options = {
		.color_transform = cursor->color_transform,
	};
	struct nx_render_pass *pass =
		nx_renderer_begin_buffer_pass(output->renderer, buffer, &options);
	if (pass == NULL) {
		return false;
	}

	enum wl_output_transform transform = nx_output_transform_compose(
		nx_output_transform_invert(cursor->transform), output->transform);

	nx_render_pass_add_rect(pass, &(struct nx_render_rect_options){
		.box = { .width = buffer->width, .height = buffer->height },
		.blend_mode = NX_RENDER_BLEND_MODE_NONE,
	});
	nx_render_pass_add_texture(pass, &(struct nx_render_texture_options){
		.texture = cursor->texture,
		.src_box = cursor->src_box,
		.dst_box = dst_box,
		.transform = transform,
		.wait_timeline = cursor->wait_timeline,
		.wait_point = cursor->wait_point,
	});
	return nx_render_pass_submit(pass);
}

struct nx_buffer *output_cursor_render_buffer(struct nx_output_cursor *cursor) {
	struct nx_output *output = cursor->output;
	if (cursor->texture == NULL) {
		return NULL;
	}

	assert(output->allocator != NULL && output->renderer != NULL);

	int width, height;
	if (!pick_cursor_size(cursor, &width, &height) ||
			!ensure_cursor_swapchain(output, width, height)) {
		return NULL;
	}

	struct nx_buffer *buffer = nx_swapchain_acquire(output->cursor_swapchain);
	if (buffer == NULL) {
		return NULL;
	}
	if (!draw_cursor(cursor, buffer)) {
		nx_buffer_unlock(buffer);
		return NULL;
	}
	return buffer;
}
