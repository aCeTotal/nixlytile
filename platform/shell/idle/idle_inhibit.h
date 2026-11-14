#ifndef PLATFORM_SHELL_IDLE_IDLE_INHIBIT_H
#define PLATFORM_SHELL_IDLE_IDLE_INHIBIT_H

#include <wayland-server-core.h>

// Visible surfaces blocking idle.
struct nx_idle_inhibit_manager_v1 {
	struct wl_list inhibitors; // nx_idle_inhibitor_v1.link
	struct wl_global *global;

	struct {
		struct wl_signal new_inhibitor; // struct nx_idle_inhibitor_v1
	} events;

	struct {
		struct wl_listener display_destroy;
	};
};

struct nx_idle_inhibitor_v1 {
	struct nx_surface *surface;
	struct wl_resource *resource;

	struct wl_list link; // nx_idle_inhibit_manager_v1.inhibitors

	struct {
		struct wl_signal destroy;
	} events;

	struct {
		struct wl_listener surface_destroy;
	};
};

struct nx_idle_inhibit_manager_v1 *nx_idle_inhibit_v1_create(struct wl_display *display);

#endif
