#ifndef PLATFORM_SURFACE_ALPHA_MODIFIER_H
#define PLATFORM_SURFACE_ALPHA_MODIFIER_H

#include <wayland-server-core.h>

struct nx_surface;
struct nx_alpha_modifier_v1;

struct nx_alpha_modifier_surface_v1_state {
	double multiplier; // between 0 and 1
};

struct nx_alpha_modifier_v1 *nx_alpha_modifier_v1_create(struct wl_display *display);

const struct nx_alpha_modifier_surface_v1_state *nx_alpha_modifier_v1_get_surface_state(
	struct nx_surface *surface);

#endif
