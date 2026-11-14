#ifndef PLATFORM_INPUT_KEYBOARD_VIRTUAL_KEYBOARD_H
#define PLATFORM_INPUT_KEYBOARD_VIRTUAL_KEYBOARD_H

#include <wayland-server-core.h>
#include "platform/input/keyboard/keyboard.h"

struct nx_virtual_keyboard_manager_v1 {
	struct wl_global *global;

	struct {
		struct wl_signal new_virtual_keyboard; // struct nx_virtual_keyboard_v1
	} events;

	struct {
		struct wl_listener display_destroy;
	};
};

struct nx_virtual_keyboard_v1 {
	struct nx_keyboard keyboard;
	struct wl_resource *resource;
	bool has_keymap;

	struct {
		struct wl_listener seat_destroy;
	};
};

struct nx_virtual_keyboard_manager_v1 *nx_virtual_keyboard_manager_v1_create(
	struct wl_display *display);

#endif
