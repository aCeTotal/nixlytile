#ifndef PLATFORM_INPUT_CURSOR_CURSOR_SHAPE_H
#define PLATFORM_INPUT_CURSOR_CURSOR_SHAPE_H

#include <wayland-server-core.h>
#include <wayland-protocols/cursor-shape-v1-enum.h>

// Handle like seat request_set_cursor.
struct nx_cursor_shape_manager_v1 {
	struct wl_global *global;

	struct {
		struct wl_signal request_set_shape; // nx_cursor_shape_manager_v1_request_set_shape_event
	} events;

	struct wl_listener display_destroy;
};

struct nx_cursor_shape_manager_v1_request_set_shape_event {
	struct nx_seat_client *seat_client;
	enum wp_cursor_shape_device_v1_shape shape;
};

struct nx_cursor_shape_manager_v1 *nx_cursor_shape_manager_v1_create(
		struct wl_display *display, uint32_t version);

// Xcursor name of a shape.
const char *nx_cursor_shape_v1_name(enum wp_cursor_shape_device_v1_shape shape);

#endif
