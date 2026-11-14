#ifndef PLATFORM_INPUT_POINTER_VIRTUAL_POINTER_H
#define PLATFORM_INPUT_POINTER_VIRTUAL_POINTER_H

#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include "platform/input/pointer/pointer_interface.h"
#include "platform/output/output_interface.h"

#define NX_VIRTUAL_POINTER_AXIS_COUNT 2

struct nx_virtual_pointer_manager_v1 {
	struct wl_global *global;
	struct wl_list virtual_pointers; // nx_virtual_pointer_v1.link

	struct {
		struct wl_signal new_virtual_pointer; // struct nx_virtual_pointer_v1_new_pointer_event
	} events;

	struct {
		struct wl_listener display_destroy;
	};
};

struct nx_virtual_pointer_v1 {
	struct nx_pointer pointer;
	struct wl_resource *resource;
	// Per axis, sent on frame.
	struct nx_pointer_axis_event axis_event[NX_VIRTUAL_POINTER_AXIS_COUNT];
	bool axis_valid[NX_VIRTUAL_POINTER_AXIS_COUNT];

	struct wl_list link; // nx_virtual_pointer_manager_v1.virtual_pointers
};

struct nx_virtual_pointer_v1_new_pointer_event {
	struct nx_virtual_pointer_v1 *new_pointer;
	struct nx_output *suggested_output; // may be NULL
};

struct nx_virtual_pointer_manager_v1 *nx_virtual_pointer_manager_v1_create(
	struct wl_display *display);

#endif
