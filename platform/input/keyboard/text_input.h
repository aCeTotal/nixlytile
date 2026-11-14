#ifndef PLATFORM_INPUT_KEYBOARD_TEXT_INPUT_H
#define PLATFORM_INPUT_KEYBOARD_TEXT_INPUT_H

#include <wayland-server-core.h>
#include "platform/input/seat/seat.h"

struct nx_surface;

// Only tracks focus and enablement.
struct nx_text_input_v3 {
	struct wl_resource *resource;
	struct nx_surface *focused_surface;
	bool pending_enabled;
	bool current_enabled;

	struct {
		struct wl_signal enable;
		struct wl_signal commit;
		struct wl_signal disable;
		struct wl_signal destroy;
	} events;

	struct {
		struct wl_listener surface_destroy;
		struct wl_listener seat_destroy;
	};
};

struct nx_text_input_manager_v3 {
	struct wl_global *global;

	struct {
		struct wl_signal new_text_input; // struct nx_text_input_v3
	} events;

	struct {
		struct wl_listener display_destroy;
	};
};

struct nx_text_input_manager_v3 *nx_text_input_manager_v3_create(
	struct wl_display *wl_display);

// Sends enter, tracks surface.
void nx_text_input_v3_send_enter(struct nx_text_input_v3 *text_input,
	struct nx_surface *nx_surface);
// Sends leave, forgets surface.
void nx_text_input_v3_send_leave(struct nx_text_input_v3 *text_input);

#endif
