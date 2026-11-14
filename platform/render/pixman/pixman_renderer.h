#ifndef PLATFORM_RENDER_PIXMAN_PIXMAN_RENDERER_H
#define PLATFORM_RENDER_PIXMAN_PIXMAN_RENDERER_H

#include <pixman.h>
#include "platform/render/drm_format_set.h"
#include "platform/render/pixel_format.h"
#include "platform/render/renderer.h"
#include "platform/render/renderer_interface.h"

struct nx_pixman_renderer {
	struct nx_renderer nx_renderer;

	struct wl_list buffers; // nx_pixman_buffer.link
	struct wl_list textures; // nx_pixman_texture.link

	struct nx_drm_format_set drm_formats;
};

/* Cached render target per buffer */
struct nx_pixman_buffer {
	struct nx_buffer *buffer;
	pixman_image_t *image;

	struct wl_listener buffer_destroy;
	struct wl_list link; // nx_pixman_renderer.buffers
};

struct nx_pixman_texture {
	struct nx_texture nx_texture;
	struct wl_list link; // nx_pixman_renderer.textures

	pixman_image_t *image;
	pixman_format_code_t format;

	struct nx_buffer *buffer; // locked source
};

struct nx_pixman_render_pass {
	struct nx_render_pass base;
	struct nx_pixman_buffer *buffer;
};

struct nx_renderer *nx_pixman_renderer_create(void);

pixman_format_code_t get_pixman_format_from_drm(uint32_t fmt);
uint32_t get_drm_format_from_pixman(pixman_format_code_t fmt);
const uint32_t *get_pixman_drm_formats(size_t *len);

/* Recreates *image_ptr when data moves */
bool begin_pixman_data_ptr_access(struct nx_buffer *buffer, pixman_image_t **image_ptr,
	uint32_t flags);

struct nx_pixman_render_pass *begin_pixman_render_pass(
	struct nx_pixman_buffer *buffer);

struct nx_texture *nx_pixman_texture_create(struct nx_pixman_renderer *renderer,
	struct nx_buffer *buffer);
struct nx_pixman_texture *nx_pixman_texture_get(struct nx_texture *texture);

#endif
