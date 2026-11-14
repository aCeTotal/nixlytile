#include <assert.h>
#include <drm_fourcc.h>
#include <pixman.h>
#include <stdlib.h>
#include <wayland-util.h>
#include "platform/buffer/buffer.h"
#include "platform/log/log.h"
#include "platform/render/pixman/pixman_renderer.h"

static const struct nx_renderer_impl renderer_impl;

static struct nx_pixman_renderer *get_renderer(
		struct nx_renderer *nx_renderer) {
	assert(nx_renderer->impl == &renderer_impl);
	struct nx_pixman_renderer *renderer = wl_container_of(nx_renderer, renderer, nx_renderer);
	return renderer;
}

bool begin_pixman_data_ptr_access(struct nx_buffer *nx_buffer, pixman_image_t **image_ptr,
		uint32_t flags) {
	pixman_image_t *image = *image_ptr;

	void *data = NULL;
	uint32_t drm_format;
	size_t stride;
	if (!nx_buffer_begin_data_ptr_access(nx_buffer, flags,
			&data, &drm_format, &stride)) {
		return false;
	}

	if (data == pixman_image_get_data(image)) {
		return true;
	}

	pixman_format_code_t format = get_pixman_format_from_drm(drm_format);
	assert(format != 0);

	pixman_image_t *new_image = pixman_image_create_bits_no_clear(format,
		nx_buffer->width, nx_buffer->height, data, stride);
	if (new_image == NULL) {
		nx_buffer_end_data_ptr_access(nx_buffer);
		return false;
	}

	pixman_image_unref(image);
	*image_ptr = new_image;
	return true;
}

static struct nx_pixman_buffer *get_buffer(
		struct nx_pixman_renderer *renderer, struct nx_buffer *nx_buffer) {
	struct nx_pixman_buffer *buffer;
	wl_list_for_each(buffer, &renderer->buffers, link) {
		if (buffer->buffer == nx_buffer) {
			return buffer;
		}
	}
	return NULL;
}

static void destroy_buffer(struct nx_pixman_buffer *buffer) {
	wl_list_remove(&buffer->link);
	wl_list_remove(&buffer->buffer_destroy.link);

	pixman_image_unref(buffer->image);

	free(buffer);
}

static void handle_destroy_buffer(struct wl_listener *listener, void *data) {
	struct nx_pixman_buffer *buffer =
		wl_container_of(listener, buffer, buffer_destroy);
	destroy_buffer(buffer);
}

static struct nx_pixman_buffer *create_buffer(
		struct nx_pixman_renderer *renderer, struct nx_buffer *nx_buffer) {
	struct nx_pixman_buffer *buffer = calloc(1, sizeof(*buffer));
	if (buffer == NULL) {
		nx_log_errno(NX_ERROR, "Allocation failed");
		return NULL;
	}
	buffer->buffer = nx_buffer;

	void *data = NULL;
	uint32_t drm_format;
	size_t stride;
	if (!nx_buffer_begin_data_ptr_access(nx_buffer,
			NX_BUFFER_DATA_PTR_ACCESS_READ | NX_BUFFER_DATA_PTR_ACCESS_WRITE,
			&data, &drm_format, &stride)) {
		nx_log(NX_ERROR, "Failed to get buffer data");
		goto error_buffer;
	}
	nx_buffer_end_data_ptr_access(nx_buffer);

	pixman_format_code_t format = get_pixman_format_from_drm(drm_format);
	if (format == 0) {
		nx_log(NX_ERROR, "Unsupported pixman drm format 0x%"PRIX32,
				drm_format);
		goto error_buffer;
	}

	buffer->image = pixman_image_create_bits(format, nx_buffer->width,
			nx_buffer->height, data, stride);
	if (!buffer->image) {
		nx_log(NX_ERROR, "Failed to allocate pixman image");
		goto error_buffer;
	}

	buffer->buffer_destroy.notify = handle_destroy_buffer;
	wl_signal_add(&nx_buffer->events.destroy, &buffer->buffer_destroy);

	wl_list_insert(&renderer->buffers, &buffer->link);

	nx_log(NX_DEBUG, "Created pixman buffer %dx%d",
		nx_buffer->width, nx_buffer->height);

	return buffer;

error_buffer:
	free(buffer);
	return NULL;
}

static const struct nx_drm_format_set *pixman_get_texture_formats(
		struct nx_renderer *nx_renderer, uint32_t buffer_caps) {
	struct nx_pixman_renderer *renderer = get_renderer(nx_renderer);
	if (!(buffer_caps & NX_BUFFER_CAP_DATA_PTR)) {
		return NULL;
	}
	return &renderer->drm_formats;
}

static const struct nx_drm_format_set *pixman_get_render_formats(
		struct nx_renderer *nx_renderer) {
	struct nx_pixman_renderer *renderer = get_renderer(nx_renderer);
	return &renderer->drm_formats;
}

static struct nx_texture *pixman_texture_from_buffer(
		struct nx_renderer *nx_renderer, struct nx_buffer *buffer) {
	return nx_pixman_texture_create(get_renderer(nx_renderer), buffer);
}

static void pixman_destroy(struct nx_renderer *nx_renderer) {
	struct nx_pixman_renderer *renderer = get_renderer(nx_renderer);

	struct nx_pixman_buffer *buffer, *buffer_tmp;
	wl_list_for_each_safe(buffer, buffer_tmp, &renderer->buffers, link) {
		destroy_buffer(buffer);
	}

	struct nx_pixman_texture *tex, *tex_tmp;
	wl_list_for_each_safe(tex, tex_tmp, &renderer->textures, link) {
		nx_texture_destroy(&tex->nx_texture);
	}

	nx_drm_format_set_finish(&renderer->drm_formats);

	free(renderer);
}

static struct nx_render_pass *pixman_begin_buffer_pass(struct nx_renderer *nx_renderer,
		struct nx_buffer *nx_buffer, const struct nx_buffer_pass_options *options) {
	struct nx_pixman_renderer *renderer = get_renderer(nx_renderer);

	struct nx_pixman_buffer *buffer = get_buffer(renderer, nx_buffer);
	if (buffer == NULL) {
		buffer = create_buffer(renderer, nx_buffer);
	}
	if (buffer == NULL) {
		return NULL;
	}

	struct nx_pixman_render_pass *pass = begin_pixman_render_pass(buffer);
	if (pass == NULL) {
		return NULL;
	}
	return &pass->base;
}

static const struct nx_renderer_impl renderer_impl = {
	.get_texture_formats = pixman_get_texture_formats,
	.get_render_formats = pixman_get_render_formats,
	.texture_from_buffer = pixman_texture_from_buffer,
	.destroy = pixman_destroy,
	.begin_buffer_pass = pixman_begin_buffer_pass,
};

struct nx_renderer *nx_pixman_renderer_create(void) {
	struct nx_pixman_renderer *renderer = calloc(1, sizeof(*renderer));
	if (renderer == NULL) {
		return NULL;
	}

	nx_log(NX_INFO, "Creating pixman renderer");
	nx_renderer_init(&renderer->nx_renderer, &renderer_impl, NX_BUFFER_CAP_DATA_PTR);
	wl_list_init(&renderer->buffers);
	wl_list_init(&renderer->textures);

	size_t len = 0;
	const uint32_t *formats = get_pixman_drm_formats(&len);

	for (size_t i = 0; i < len; ++i) {
		nx_drm_format_set_add(&renderer->drm_formats, formats[i],
			DRM_FORMAT_MOD_INVALID);
		nx_drm_format_set_add(&renderer->drm_formats, formats[i],
			DRM_FORMAT_MOD_LINEAR);
	}

	return &renderer->nx_renderer;
}
