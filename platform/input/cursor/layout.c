#include "platform/input/cursor/state.h"
#include "platform/output/output.h"

static void detach_output_layout(struct nx_cursor_state *state) {
	if (state->layout == NULL) {
		return;
	}

	struct nx_cursor_output_cursor *output_cursor, *tmp;
	wl_list_for_each_safe(output_cursor, tmp, &state->output_cursors, link) {
		cursor_output_destroy(output_cursor);
	}

	wl_list_remove(&state->layout_destroy.link);
	wl_list_remove(&state->layout_change.link);
	wl_list_remove(&state->layout_add.link);
	state->layout = NULL;
}

static void handle_layout_destroy(struct wl_listener *listener, void *data) {
	struct nx_cursor_state *state =
		wl_container_of(listener, state, layout_destroy);
	detach_output_layout(state);
}

static void layout_add(struct nx_cursor_state *state,
		struct nx_output_layout_output *l_output) {
	struct nx_cursor_output_cursor *output_cursor;
	wl_list_for_each(output_cursor, &state->output_cursors, link) {
		if (output_cursor->output_cursor->output == l_output->output) {
			return;
		}
	}
	cursor_output_create(state, l_output);
}

static void handle_layout_add(struct wl_listener *listener, void *data) {
	struct nx_cursor_state *state = wl_container_of(listener, state, layout_add);
	layout_add(state, data);
}

// Pulls the cursor back on-screen.
static void handle_layout_change(struct wl_listener *listener, void *data) {
	struct nx_cursor_state *state =
		wl_container_of(listener, state, layout_change);
	struct nx_output_layout *layout = data;
	struct nx_cursor *cur = &state->cursor;

	if (nx_output_layout_contains_point(layout, NULL, (int)cur->x, (int)cur->y) ||
			wl_list_empty(&layout->outputs)) {
		return;
	}
	double x, y;
	nx_output_layout_closest_point(layout, NULL, cur->x, cur->y, &x, &y);
	cursor_warp_unchecked(cur, x, y);
}

void nx_cursor_attach_output_layout(struct nx_cursor *cur,
		struct nx_output_layout *layout) {
	struct nx_cursor_state *state = cur->state;
	detach_output_layout(state);
	if (layout == NULL) {
		return;
	}

	wl_signal_add(&layout->events.add, &state->layout_add);
	state->layout_add.notify = handle_layout_add;
	wl_signal_add(&layout->events.change, &state->layout_change);
	state->layout_change.notify = handle_layout_change;
	wl_signal_add(&layout->events.destroy, &state->layout_destroy);
	state->layout_destroy.notify = handle_layout_destroy;
	state->layout = layout;

	struct nx_output_layout_output *l_output;
	wl_list_for_each(l_output, &layout->outputs, link) {
		layout_add(state, l_output);
	}
}
