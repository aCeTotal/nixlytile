#ifndef PLATFORM_BUFFER_SINGLE_PIXEL_H
#define PLATFORM_BUFFER_SINGLE_PIXEL_H

#include <wayland-server-core.h>

struct nx_single_pixel_buffer_manager_v1 *nx_single_pixel_buffer_manager_v1_create(
	struct wl_display *display);

#endif
