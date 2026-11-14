#ifndef PLATFORM_INPUT_CURSOR_STATE_H
#define PLATFORM_INPUT_CURSOR_STATE_H

#include <wayland-server-core.h>
#include "platform/input/cursor/cursor.h"

// The cursor's image on one output.
struct nx_cursor_output_cursor {
	struct nx_cursor *cursor;
	struct nx_output_cursor *output_cursor;
	struct wl_list link;

	struct wl_listener layout_output_destroy;
	struct wl_listener output_commit;

	// Xcursor animation state.
	struct nx_xcursor *xcursor;
	size_t xcursor_index;
	struct wl_event_source *xcursor_timer;
};

// One image source at most.
struct nx_cursor_state {
	struct nx_cursor cursor;

	struct wl_list devices; // nx_cursor_device.link
	struct wl_list output_cursors; // nx_cursor_output_cursor.link
	struct nx_output_layout *layout;

	struct wl_listener layout_add;
	struct wl_listener layout_change;
	struct wl_listener layout_destroy;

	struct nx_buffer *buffer;
	struct {
		int32_t x, y;
	} buffer_hotspot;
	float buffer_scale;

	struct nx_surface *surface;
	struct {
		int32_t x, y;
	} surface_hotspot;
	struct wl_listener surface_commit;
	struct wl_listener surface_destroy;

	struct nx_xcursor_manager *xcursor_manager;
	char *xcursor_name;
};

void cursor_warp_unchecked(struct nx_cursor *cur, double lx, double ly);
struct nx_output *cursor_device_mapped_output(struct nx_cursor *cur,
		struct nx_input_device *dev);

void cursor_output_create(struct nx_cursor_state *state,
		struct nx_output_layout_output *l_output);
void cursor_output_destroy(struct nx_cursor_output_cursor *output_cursor);
void cursor_output_move(struct nx_cursor_output_cursor *output_cursor);
void cursor_output_update(struct nx_cursor_output_cursor *output_cursor);

#endif
