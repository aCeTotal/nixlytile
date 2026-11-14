#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "platform/buffer/buffer.h"
#include "platform/input/cursor/state.h"
#include "platform/output/output.h"
#include "platform/surface/compositor.h"

static void update_outputs(struct nx_cursor *cur) {
	struct nx_cursor_output_cursor *output_cursor;
	wl_list_for_each(output_cursor, &cur->state->output_cursors, link) {
		cursor_output_update(output_cursor);
	}
}

static void surface_leave_outputs(struct nx_cursor_state *state) {
	if (state->surface == NULL) {
		return;
	}
	struct nx_cursor_output_cursor *output_cursor;
	wl_list_for_each(output_cursor, &state->output_cursors, link) {
		nx_surface_send_leave(state->surface,
			output_cursor->output_cursor->output);
	}
}

static void reset_image(struct nx_cursor *cur) {
	struct nx_cursor_state *state = cur->state;
	nx_buffer_unlock(state->buffer);
	state->buffer = NULL;

	surface_leave_outputs(state);
	wl_list_remove(&state->surface_destroy.link);
	wl_list_remove(&state->surface_commit.link);
	wl_list_init(&state->surface_destroy.link);
	wl_list_init(&state->surface_commit.link);
	state->surface = NULL;

	state->xcursor_manager = NULL;
	free(state->xcursor_name);
	state->xcursor_name = NULL;
}

static void unset_image(struct nx_cursor *cur) {
	reset_image(cur);
	update_outputs(cur);
}

void nx_cursor_set_buffer(struct nx_cursor *cur, struct nx_buffer *buffer,
		int32_t hotspot_x, int32_t hotspot_y, float scale) {
	struct nx_cursor_state *state = cur->state;
	if (buffer == state->buffer &&
			hotspot_x == state->buffer_hotspot.x &&
			hotspot_y == state->buffer_hotspot.y &&
			scale == state->buffer_scale) {
		return;
	}

	reset_image(cur);
	if (buffer != NULL) {
		state->buffer = nx_buffer_lock(buffer);
		state->buffer_hotspot.x = hotspot_x;
		state->buffer_hotspot.y = hotspot_y;
		state->buffer_scale = scale;
	}
	update_outputs(cur);
}

void nx_cursor_set_xcursor(struct nx_cursor *cur,
		struct nx_xcursor_manager *manager, const char *name) {
	struct nx_cursor_state *state = cur->state;
	if (manager == state->xcursor_manager && state->xcursor_name != NULL &&
			strcmp(name, state->xcursor_name) == 0) {
		return;
	}

	reset_image(cur);
	state->xcursor_manager = manager;
	state->xcursor_name = strdup(name);
	update_outputs(cur);
}

static void handle_surface_destroy(struct wl_listener *listener, void *data) {
	struct nx_cursor_state *state =
		wl_container_of(listener, state, surface_destroy);
	assert(state->surface != NULL);
	unset_image(&state->cursor);
}

static void handle_surface_commit(struct wl_listener *listener, void *data) {
	struct nx_cursor_state *state =
		wl_container_of(listener, state, surface_commit);
	state->surface_hotspot.x -= state->surface->current.dx;
	state->surface_hotspot.y -= state->surface->current.dy;
	update_outputs(&state->cursor);
}

static void switch_surface(struct nx_cursor *cur, struct nx_surface *surface) {
	struct nx_cursor_state *state = cur->state;
	reset_image(cur);
	state->surface = surface;
	wl_signal_add(&surface->events.destroy, &state->surface_destroy);
	state->surface_destroy.notify = handle_surface_destroy;
	wl_signal_add(&surface->events.commit, &state->surface_commit);
	state->surface_commit.notify = handle_surface_commit;
}

void nx_cursor_set_surface(struct nx_cursor *cur, struct nx_surface *surface,
		int32_t hotspot_x, int32_t hotspot_y) {
	struct nx_cursor_state *state = cur->state;
	if (surface == NULL) {
		unset_image(cur);
		return;
	}
	if (surface == state->surface &&
			hotspot_x == state->surface_hotspot.x &&
			hotspot_y == state->surface_hotspot.y) {
		return;
	}

	if (surface != state->surface) {
		switch_surface(cur, surface);
	}
	state->surface_hotspot.x = hotspot_x;
	state->surface_hotspot.y = hotspot_y;
	update_outputs(cur);
}
