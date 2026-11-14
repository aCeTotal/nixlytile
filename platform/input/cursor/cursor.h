#ifndef PLATFORM_INPUT_CURSOR_CURSOR_H
#define PLATFORM_INPUT_CURSOR_CURSOR_H

#include <wayland-server-core.h>
#include "platform/output/layout.h"

struct nx_buffer;
struct nx_cursor_state;
struct nx_input_device;
struct nx_surface;
struct nx_xcursor_manager;

// Layout-wide pointer position and image.
struct nx_cursor {
	struct nx_cursor_state *state;
	double x, y;

	// Re-emitted from attached pointers.
	struct {
		struct wl_signal motion; // nx_pointer_motion_event
		struct wl_signal motion_absolute; // nx_pointer_motion_absolute_event
		struct wl_signal button; // nx_pointer_button_event
		struct wl_signal axis; // nx_pointer_axis_event
		struct wl_signal frame;
	} events;
};

struct nx_cursor *nx_cursor_create(void);

// NULL `dev` ignores mappings.
bool nx_cursor_warp(struct nx_cursor *cur, struct nx_input_device *dev,
		double lx, double ly);
void nx_cursor_warp_closest(struct nx_cursor *cur,
		struct nx_input_device *dev, double lx, double ly);
void nx_cursor_warp_absolute(struct nx_cursor *cur,
		struct nx_input_device *dev, double x, double y);
void nx_cursor_absolute_to_layout_coords(struct nx_cursor *cur,
		struct nx_input_device *dev, double x, double y, double *lx, double *ly);
void nx_cursor_move(struct nx_cursor *cur, struct nx_input_device *dev,
		double delta_x, double delta_y);

// Logical hotspot; NULL hides.
void nx_cursor_set_buffer(struct nx_cursor *cur, struct nx_buffer *buffer,
		int32_t hotspot_x, int32_t hotspot_y, float scale);
void nx_cursor_set_xcursor(struct nx_cursor *cur,
		struct nx_xcursor_manager *manager, const char *name);
void nx_cursor_set_surface(struct nx_cursor *cur, struct nx_surface *surface,
		int32_t hotspot_x, int32_t hotspot_y);

void nx_cursor_attach_input_device(struct nx_cursor *cur,
		struct nx_input_device *dev);
void nx_cursor_map_input_to_output(struct nx_cursor *cur,
		struct nx_input_device *dev, struct nx_output *output);
void nx_cursor_attach_output_layout(struct nx_cursor *cur,
		struct nx_output_layout *layout);

#endif
