#ifndef PLATFORM_SURFACE_TEARING_CONTROL_H
#define PLATFORM_SURFACE_TEARING_CONTROL_H

#include <wayland-server-core.h>
#include <wayland-protocols/tearing-control-v1-enum.h>

struct nx_surface;
struct nx_tearing_control_manager_v1;

struct nx_tearing_control_manager_v1 *nx_tearing_control_manager_v1_create(
	struct wl_display *display, uint32_t version);

enum wp_tearing_control_v1_presentation_hint
nx_tearing_control_manager_v1_surface_hint_from_surface(
	struct nx_tearing_control_manager_v1 *manager,
	struct nx_surface *surface);

#endif
