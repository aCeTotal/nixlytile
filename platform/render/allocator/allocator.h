#ifndef PLATFORM_RENDER_ALLOCATOR_ALLOCATOR_H
#define PLATFORM_RENDER_ALLOCATOR_ALLOCATOR_H

#include <wayland-server-core.h>

struct nx_allocator;
struct nx_backend;
struct nx_drm_format;
struct nx_renderer;

struct nx_allocator_interface {
	struct nx_buffer *(*create_buffer)(struct nx_allocator *alloc,
		int width, int height, const struct nx_drm_format *format);
	void (*destroy)(struct nx_allocator *alloc);
};

void nx_allocator_init(struct nx_allocator *alloc,
	const struct nx_allocator_interface *impl, uint32_t buffer_caps);

/* Allocates pixel buffers for swapchains */
struct nx_allocator {
	const struct nx_allocator_interface *impl;

	uint32_t buffer_caps; // of created buffers

	struct {
		struct wl_signal destroy;
	} events;
};

/* Tries gbm, shm, then dumb */
struct nx_allocator *nx_allocator_autocreate(struct nx_backend *backend,
	struct nx_renderer *renderer);

void nx_allocator_destroy(struct nx_allocator *alloc);

/* Uses a modifier from format */
struct nx_buffer *nx_allocator_create_buffer(struct nx_allocator *alloc,
	int width, int height, const struct nx_drm_format *format);

#endif
