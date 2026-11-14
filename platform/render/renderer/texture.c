#include <assert.h>
#include "platform/render/renderer_interface.h"
#include "platform/render/texture.h"
#include "platform/render/pixel_format.h"

void nx_texture_init(struct nx_texture *texture, struct nx_renderer *renderer,
		const struct nx_texture_impl *impl, uint32_t width, uint32_t height) {
	assert(renderer);
	assert(impl->read_pixels);
	assert(impl->preferred_read_format);
	assert(impl->destroy);

	*texture = (struct nx_texture){
		.renderer = renderer,
		.impl = impl,
		.width = width,
		.height = height,
	};
}

void nx_texture_destroy(struct nx_texture *texture) {
	if (!texture) {
		return;
	}
	texture->impl->destroy(texture);
}

void nx_texture_read_pixels_options_get_src_box(
		const struct nx_texture_read_pixels_options *options,
		const struct nx_texture *texture, struct nx_box *box) {
	if (nx_box_empty(&options->src_box)) {
		*box = (struct nx_box){
			.x = 0,
			.y = 0,
			.width = texture->width,
			.height = texture->height,
		};
		return;
	}

	*box = options->src_box;
}

void *nx_texture_read_pixel_options_get_data(
		const struct nx_texture_read_pixels_options *options) {
	const struct nx_pixel_format_info *fmt = drm_get_pixel_format_info(options->format);

	return (char *)options->data +
		pixel_format_info_min_stride(fmt, options->dst_x) +
		options->dst_y * options->stride;
}

bool nx_texture_read_pixels(struct nx_texture *texture,
		const struct nx_texture_read_pixels_options *options) {
	return texture->impl->read_pixels(texture, options);
}

uint32_t nx_texture_preferred_read_format(struct nx_texture *texture) {
	return texture->impl->preferred_read_format(texture);
}

struct nx_texture *nx_texture_from_buffer(struct nx_renderer *renderer,
		struct nx_buffer *buffer) {
	return renderer->impl->texture_from_buffer(renderer, buffer);
}

bool nx_texture_update_from_buffer(struct nx_texture *texture,
		struct nx_buffer *buffer, const pixman_region32_t *damage) {
	if (!texture->impl->update_from_buffer) {
		return false;
	}
	if (texture->width != (uint32_t)buffer->width ||
			texture->height != (uint32_t)buffer->height) {
		return false;
	}
	const pixman_box32_t *extents = pixman_region32_extents(damage);
	if (extents->x1 < 0 || extents->y1 < 0 || extents->x2 > buffer->width ||
			extents->y2 > buffer->height) {
		return false;
	}
	return texture->impl->update_from_buffer(texture, buffer, damage);
}
