#ifndef PLATFORM_RENDER_RENDERER_INTERFACE_H
#define PLATFORM_RENDER_RENDERER_INTERFACE_H

#include <stdbool.h>
#include <wayland-server-core.h>
#include "platform/render/renderer.h"
#include "platform/render/texture.h"
#include "platform/output/output.h"
#include "platform/render/dmabuf.h"

struct nx_renderer_impl {
	const struct nx_drm_format_set *(*get_texture_formats)(
		struct nx_renderer *renderer, uint32_t buffer_caps);
	const struct nx_drm_format_set *(*get_render_formats)(
		struct nx_renderer *renderer);
	void (*destroy)(struct nx_renderer *renderer);
	int (*get_drm_fd)(struct nx_renderer *renderer); // optional
	struct nx_texture *(*texture_from_buffer)(struct nx_renderer *renderer,
		struct nx_buffer *buffer);
	struct nx_render_pass *(*begin_buffer_pass)(struct nx_renderer *renderer,
		struct nx_buffer *buffer, const struct nx_buffer_pass_options *options);
};

void nx_renderer_init(struct nx_renderer *renderer,
	const struct nx_renderer_impl *impl, uint32_t render_buffer_caps);

struct nx_texture_impl {
	bool (*update_from_buffer)(struct nx_texture *texture,
		struct nx_buffer *buffer, const pixman_region32_t *damage); // optional
	bool (*read_pixels)(struct nx_texture *texture,
		const struct nx_texture_read_pixels_options *options);
	uint32_t (*preferred_read_format)(struct nx_texture *texture);
	void (*destroy)(struct nx_texture *texture);
};

void nx_texture_init(struct nx_texture *texture, struct nx_renderer *renderer,
	const struct nx_texture_impl *impl, uint32_t width, uint32_t height);

struct nx_render_pass {
	const struct nx_render_pass_impl *impl;
};

void nx_render_pass_init(struct nx_render_pass *pass,
	const struct nx_render_pass_impl *impl);

struct nx_render_pass_impl {
	bool (*submit)(struct nx_render_pass *pass);
	void (*add_texture)(struct nx_render_pass *pass,
		const struct nx_render_texture_options *options);
	void (*add_rect)(struct nx_render_pass *pass,
		const struct nx_render_rect_options *options);
};

void nx_render_texture_options_get_src_box(const struct nx_render_texture_options *options,
	struct nx_fbox *box);
void nx_render_texture_options_get_dst_box(const struct nx_render_texture_options *options,
	struct nx_box *box);
float nx_render_texture_options_get_alpha(const struct nx_render_texture_options *options);
void nx_render_rect_options_get_box(const struct nx_render_rect_options *options,
	const struct nx_buffer *buffer, struct nx_box *box);

void nx_texture_read_pixels_options_get_src_box(
	const struct nx_texture_read_pixels_options *options,
	const struct nx_texture *texture, struct nx_box *box);
void *nx_texture_read_pixel_options_get_data(
	const struct nx_texture_read_pixels_options *options);

#endif
