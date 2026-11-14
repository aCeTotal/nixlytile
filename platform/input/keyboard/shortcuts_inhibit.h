#ifndef PLATFORM_INPUT_KEYBOARD_SHORTCUTS_INHIBIT_H
#define PLATFORM_INPUT_KEYBOARD_SHORTCUTS_INHIBIT_H

#include <wayland-server-core.h>
#include "platform/input/seat/seat.h"

struct nx_keyboard_shortcuts_inhibit_manager_v1 {
	struct wl_list inhibitors; // nx_keyboard_shortcuts_inhibitor_v1.link
	struct wl_global *global;

	struct {
		struct wl_signal new_inhibitor; // struct nx_keyboard_shortcuts_inhibitor_v1
	} events;

	struct {
		struct wl_listener display_destroy;
	};
};

// Applies only while surface focused.
struct nx_keyboard_shortcuts_inhibitor_v1 {
	struct nx_surface *surface;
	struct nx_seat *seat;
	bool active;
	struct wl_resource *resource;

	struct wl_list link; // nx_keyboard_shortcuts_inhibit_manager_v1.inhibitors

	struct {
		struct wl_listener surface_destroy;
		struct wl_listener seat_destroy;
	};
};

struct nx_keyboard_shortcuts_inhibit_manager_v1 *
nx_keyboard_shortcuts_inhibit_v1_create(struct wl_display *display);

// Sends active once.
void nx_keyboard_shortcuts_inhibitor_v1_activate(
	struct nx_keyboard_shortcuts_inhibitor_v1 *inhibitor);

#endif
