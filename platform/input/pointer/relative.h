#ifndef PLATFORM_INPUT_POINTER_RELATIVE_H
#define PLATFORM_INPUT_POINTER_RELATIVE_H

#include <wayland-server-core.h>

struct nx_relative_pointer_manager_v1 {
	struct wl_global *global;
	struct wl_list relative_pointers; // nx_relative_pointer_v1.link

	struct {
		struct wl_listener display_destroy_listener;
	};
};

// Relative motion for focused clients.
struct nx_relative_pointer_v1 {
	struct wl_resource *resource;
	struct wl_resource *pointer_resource;
	struct nx_seat *seat;
	struct wl_list link; // nx_relative_pointer_manager_v1.relative_pointers

	struct {
		struct wl_listener seat_destroy;
		struct wl_listener pointer_destroy;
	};
};

struct nx_relative_pointer_manager_v1 *nx_relative_pointer_manager_v1_create(
	struct wl_display *display);

// Time is in microseconds.
void nx_relative_pointer_manager_v1_send_relative_motion(
	struct nx_relative_pointer_manager_v1 *manager, struct nx_seat *seat,
	uint64_t time_usec, double dx, double dy,
	double dx_unaccel, double dy_unaccel);

#endif
