#include <assert.h>
#include <stdint.h>
#include "platform/nested/output/output.h"
#include "platform/nested/seat/seat.h"

static bool attach_cursor(struct wl_surface *surface, struct nx_wl_backend *backend,
		struct nx_buffer *nx_buffer) {
	if (nx_buffer == NULL) {
		wl_surface_attach(surface, NULL, 0, 0);
		return true;
	}

	struct nx_wl_buffer *buffer = nested_buffer_get_or_create(backend, nx_buffer);
	if (buffer == NULL) {
		return false;
	}
	wl_surface_attach(surface, buffer->wl_buffer, 0, 0);
	wl_surface_damage_buffer(surface, 0, 0, INT32_MAX, INT32_MAX);
	return true;
}

bool nested_output_set_cursor(struct nx_output *nx_output,
		struct nx_buffer *nx_buffer, int hotspot_x, int hotspot_y) {
	struct nx_wl_output *output = nested_output_from_output(nx_output);
	struct nx_wl_backend *backend = output->backend;

	output->cursor.hotspot_x = hotspot_x;
	output->cursor.hotspot_y = hotspot_y;

	if (output->cursor.surface == NULL) {
		output->cursor.surface = wl_compositor_create_surface(backend->compositor);
	}
	struct wl_surface *surface = output->cursor.surface;
	if (!attach_cursor(surface, backend, nx_buffer)) {
		return false;
	}
	wl_surface_commit(surface);

	nested_output_update_cursor(output);
	wl_display_flush(backend->remote_display);
	return true;
}

// The host draws the cursor.
bool nested_output_move_cursor(struct nx_output *nx_output, int x, int y) {
	return true;
}

void nested_output_update_cursor(struct nx_wl_output *output) {
	struct nx_wl_pointer *pointer = output->cursor.pointer;
	if (pointer == NULL) {
		return;
	}
	assert(pointer->output == output);
	assert(output->enter_serial);

	wl_pointer_set_cursor(pointer->seat->wl_pointer, output->enter_serial,
		output->cursor.surface, output->cursor.hotspot_x,
		output->cursor.hotspot_y);
}
