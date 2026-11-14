#ifndef PLATFORM_SURFACE_FRACTIONAL_SCALE_H
#define PLATFORM_SURFACE_FRACTIONAL_SCALE_H

#include <wayland-server-core.h>

struct nx_surface;
struct nx_fractional_scale_manager_v1;

void nx_fractional_scale_v1_notify_scale(
	struct nx_surface *surface, double scale);

struct nx_fractional_scale_manager_v1 *nx_fractional_scale_manager_v1_create(
	struct wl_display *display, uint32_t version);

#endif
