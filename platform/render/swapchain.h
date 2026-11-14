#ifndef PLATFORM_RENDER_SWAPCHAIN_H
#define PLATFORM_RENDER_SWAPCHAIN_H

#include <stdbool.h>
#include <wayland-server-core.h>
#include "platform/render/drm_format_set.h"

#define NX_SWAPCHAIN_CAP 4

struct nx_swapchain_slot {
	struct nx_buffer *buffer;
	bool acquired; // waiting for release

	struct {
		struct wl_listener release;
	};
};

struct nx_swapchain {
	struct nx_allocator *allocator; // NULL once destroyed

	int width, height;
	struct nx_drm_format format;

	struct nx_swapchain_slot slots[NX_SWAPCHAIN_CAP];

	struct {
		struct wl_listener allocator_destroy;
	};
};

struct nx_swapchain *nx_swapchain_create(
	struct nx_allocator *alloc, int width, int height,
	const struct nx_drm_format *format);
void nx_swapchain_destroy(struct nx_swapchain *swapchain);
/* Returns a locked buffer */
struct nx_buffer *nx_swapchain_acquire(struct nx_swapchain *swapchain);

#endif
