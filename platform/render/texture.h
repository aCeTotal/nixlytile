#ifndef PLATFORM_RENDER_TEXTURE_H
#define PLATFORM_RENDER_TEXTURE_H

#include <pixman.h>
#include <stdint.h>
#include <wayland-server-core.h>
#include "platform/render/dmabuf.h"
#include "platform/geometry/box.h"

struct nx_buffer;
struct nx_renderer;
struct nx_texture_impl;

struct nx_texture {
	const struct nx_texture_impl *impl;
	uint32_t width, height;

	struct nx_renderer *renderer;
};

struct nx_texture_read_pixels_options {
	void *data;
	uint32_t format; // DRM fourcc written to data
	uint32_t stride; // bytes
	uint32_t dst_x, dst_y;
	const struct nx_box src_box; // empty means whole texture
	struct nx_drm_syncobj_timeline *wait_timeline;
	uint64_t wait_point;
};

bool nx_texture_read_pixels(struct nx_texture *texture,
	const struct nx_texture_read_pixels_options *options);

uint32_t nx_texture_preferred_read_format(struct nx_texture *texture);

/* False: recreate from buffer */
bool nx_texture_update_from_buffer(struct nx_texture *texture,
	struct nx_buffer *buffer, const pixman_region32_t *damage);

void nx_texture_destroy(struct nx_texture *texture);

struct nx_texture *nx_texture_from_buffer(struct nx_renderer *renderer,
	struct nx_buffer *buffer);

#endif
