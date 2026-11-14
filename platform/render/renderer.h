#ifndef PLATFORM_RENDER_RENDERER_H
#define PLATFORM_RENDER_RENDERER_H

#include <stdint.h>
#include <wayland-server-core.h>
#include "platform/render/pass.h"
#include "platform/render/texture.h"
#include "platform/geometry/box.h"

struct nx_backend;
struct nx_renderer_impl;
struct nx_drm_format_set;
struct nx_buffer;

struct nx_renderer {
	uint32_t render_buffer_caps; // enum nx_buffer_cap bits
	uint32_t color_encodings; // enum nx_color_encoding bits

	struct {
		struct wl_signal destroy;
		struct wl_signal lost; // GPU reset, recreate the renderer
	} events;

	struct {
		bool input_color_transform;
		bool output_color_transform;
		bool timeline; // drm_syncobj wait and signal
	} features;

	struct {
		const struct nx_renderer_impl *impl;
	};
};

struct nx_renderer *nx_renderer_autocreate(struct nx_backend *backend);
struct nx_renderer *renderer_autocreate_with_drm_fd(int drm_fd);

const struct nx_drm_format_set *nx_renderer_get_texture_formats(
	struct nx_renderer *r, uint32_t buffer_caps);
const struct nx_drm_format_set *nx_renderer_get_render_formats(
	struct nx_renderer *renderer);

bool nx_renderer_init_wl_shm(struct nx_renderer *r,
	struct wl_display *wl_display);

/* Borrowed FD, -1 if none */
int nx_renderer_get_drm_fd(struct nx_renderer *r);

/* Textures need separate destruction */
void nx_renderer_destroy(struct nx_renderer *renderer);

#endif
