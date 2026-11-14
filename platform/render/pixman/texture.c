#include <assert.h>
#include <stdlib.h>
#include "platform/buffer/buffer.h"
#include "platform/log/log.h"
#include "platform/render/pixman/pixman_renderer.h"

static const struct nx_texture_impl texture_impl;

struct nx_pixman_texture *nx_pixman_texture_get(struct nx_texture *nx_texture) {
	assert(nx_texture->impl == &texture_impl);
	struct nx_pixman_texture *texture = wl_container_of(nx_texture, texture, nx_texture);
	return texture;
}

static void texture_destroy(struct nx_texture *nx_texture) {
	struct nx_pixman_texture *texture = nx_pixman_texture_get(nx_texture);
	wl_list_remove(&texture->link);
	pixman_image_unref(texture->image);
	nx_buffer_unlock(texture->buffer);
	free(texture);
}

static bool texture_read_pixels(struct nx_texture *nx_texture,
		const struct nx_texture_read_pixels_options *options) {
	struct nx_pixman_texture *texture = nx_pixman_texture_get(nx_texture);

	struct nx_box src;
	nx_texture_read_pixels_options_get_src_box(options, nx_texture, &src);

	pixman_format_code_t fmt = get_pixman_format_from_drm(options->format);
	if (fmt == 0) {
		nx_log(NX_ERROR, "Cannot read pixels: unsupported pixel format");
		return false;
	}

	void *p = nx_texture_read_pixel_options_get_data(options);

	pixman_image_t *dst = pixman_image_create_bits_no_clear(fmt,
			src.width, src.height, p, options->stride);

	pixman_image_composite32(PIXMAN_OP_SRC, texture->image, NULL, dst,
			src.x, src.y, 0, 0, 0, 0, src.width, src.height);

	pixman_image_unref(dst);

	return true;
}

static uint32_t texture_preferred_read_format(struct nx_texture *nx_texture) {
	struct nx_pixman_texture *texture = nx_pixman_texture_get(nx_texture);

	pixman_format_code_t pixman_format = pixman_image_get_format(texture->image);
	return get_drm_format_from_pixman(pixman_format);
}

static const struct nx_texture_impl texture_impl = {
	.read_pixels = texture_read_pixels,
	.preferred_read_format = texture_preferred_read_format,
	.destroy = texture_destroy,
};

static struct nx_pixman_texture *texture_alloc(struct nx_pixman_renderer *renderer,
		uint32_t drm_format, const struct nx_buffer *buffer) {
	struct nx_pixman_texture *texture = calloc(1, sizeof(*texture));
	if (texture == NULL) {
		nx_log_errno(NX_ERROR, "Failed to allocate pixman texture");
		return NULL;
	}

	nx_texture_init(&texture->nx_texture, &renderer->nx_renderer,
		&texture_impl, buffer->width, buffer->height);

	if (drm_get_pixel_format_info(drm_format) == NULL) {
		nx_log(NX_ERROR, "Unsupported drm format 0x%"PRIX32, drm_format);
		free(texture);
		return NULL;
	}

	texture->format = get_pixman_format_from_drm(drm_format);
	if (texture->format == 0) {
		nx_log(NX_ERROR, "Unsupported pixman drm format 0x%"PRIX32,
				drm_format);
		free(texture);
		return NULL;
	}

	wl_list_insert(&renderer->textures, &texture->link);

	return texture;
}

struct nx_texture *nx_pixman_texture_create(struct nx_pixman_renderer *renderer,
		struct nx_buffer *buffer) {
	void *data = NULL;
	uint32_t drm_format;
	size_t stride;
	if (!nx_buffer_begin_data_ptr_access(buffer, NX_BUFFER_DATA_PTR_ACCESS_READ,
			&data, &drm_format, &stride)) {
		return NULL;
	}
	nx_buffer_end_data_ptr_access(buffer);

	struct nx_pixman_texture *texture = texture_alloc(renderer, drm_format, buffer);
	if (texture == NULL) {
		return NULL;
	}

	texture->image = pixman_image_create_bits_no_clear(texture->format,
		buffer->width, buffer->height, data, stride);
	if (!texture->image) {
		nx_log(NX_ERROR, "Failed to create pixman image");
		wl_list_remove(&texture->link);
		free(texture);
		return NULL;
	}

	texture->buffer = nx_buffer_lock(buffer);

	return &texture->nx_texture;
}
