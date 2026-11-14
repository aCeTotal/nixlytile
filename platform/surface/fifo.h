#ifndef PLATFORM_SURFACE_FIFO_H
#define PLATFORM_SURFACE_FIFO_H

#include <wayland-server-core.h>

struct nx_surface;
struct nx_fifo_manager_v1;

struct nx_fifo_manager_v1 *nx_fifo_manager_v1_create(
	struct wl_display *display, uint32_t version);

// Call when content is latched
void nx_fifo_v1_surface_latched(struct nx_surface *surface);

#endif
