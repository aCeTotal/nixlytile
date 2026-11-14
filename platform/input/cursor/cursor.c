#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include "platform/geometry/box.h"
#include "platform/input/cursor/state.h"
#include "platform/log/log.h"

#define CURSOR_INITIAL_POSITION 100

struct nx_cursor *nx_cursor_create(void) {
	struct nx_cursor_state *state = calloc(1, sizeof(*state));
	if (state == NULL) {
		nx_log(NX_ERROR, "Failed to allocate nx_cursor_state");
		return NULL;
	}
	struct nx_cursor *cur = &state->cursor;
	cur->state = state;

	wl_list_init(&state->devices);
	wl_list_init(&state->output_cursors);
	wl_signal_init(&cur->events.motion);
	wl_signal_init(&cur->events.motion_absolute);
	wl_signal_init(&cur->events.button);
	wl_signal_init(&cur->events.axis);
	wl_signal_init(&cur->events.frame);
	wl_list_init(&state->surface_destroy.link);
	wl_list_init(&state->surface_commit.link);

	cur->x = CURSOR_INITIAL_POSITION;
	cur->y = CURSOR_INITIAL_POSITION;
	return cur;
}

void cursor_warp_unchecked(struct nx_cursor *cur, double lx, double ly) {
	assert(cur->state->layout);
	if (!isfinite(lx) || !isfinite(ly)) {
		assert(false);
		return;
	}

	cur->x = lx;
	cur->y = ly;

	struct nx_cursor_output_cursor *output_cursor;
	wl_list_for_each(output_cursor, &cur->state->output_cursors, link) {
		cursor_output_move(output_cursor);
	}
}

// Mapped output box, or empty.
static void get_mapping(struct nx_cursor *cur, struct nx_input_device *dev,
		struct nx_box *box) {
	assert(cur->state->layout);
	*box = (struct nx_box){0};
	struct nx_output *output = cursor_device_mapped_output(cur, dev);
	if (output != NULL) {
		nx_output_layout_get_box(cur->state->layout, output, box);
	}
}

bool nx_cursor_warp(struct nx_cursor *cur, struct nx_input_device *dev,
		double lx, double ly) {
	assert(cur->state->layout);

	struct nx_box mapping;
	get_mapping(cur, dev, &mapping);
	bool inside = !nx_box_empty(&mapping) ?
		nx_box_contains_point(&mapping, lx, ly) :
		nx_output_layout_contains_point(cur->state->layout, NULL,
			(int)lx, (int)ly);
	if (inside) {
		cursor_warp_unchecked(cur, lx, ly);
	}
	return inside;
}

void nx_cursor_warp_closest(struct nx_cursor *cur,
		struct nx_input_device *dev, double lx, double ly) {
	struct nx_box mapping;
	get_mapping(cur, dev, &mapping);
	bool mapped = !nx_box_empty(&mapping);
	// No outputs: stay put.
	if (!mapped && wl_list_empty(&cur->state->layout->outputs)) {
		return;
	}

	if (mapped) {
		nx_box_closest_point(&mapping, lx, ly, &lx, &ly);
	} else {
		nx_output_layout_closest_point(cur->state->layout, NULL, lx, ly,
			&lx, &ly);
	}
	cursor_warp_unchecked(cur, lx, ly);
}

void nx_cursor_absolute_to_layout_coords(struct nx_cursor *cur,
		struct nx_input_device *dev, double x, double y,
		double *lx, double *ly) {
	assert(cur->state->layout);

	struct nx_box mapping;
	get_mapping(cur, dev, &mapping);
	if (nx_box_empty(&mapping)) {
		nx_output_layout_get_box(cur->state->layout, NULL, &mapping);
	}

	*lx = !isnan(x) ? mapping.width * x + mapping.x : cur->x;
	*ly = !isnan(y) ? mapping.height * y + mapping.y : cur->y;
}

void nx_cursor_warp_absolute(struct nx_cursor *cur,
		struct nx_input_device *dev, double x, double y) {
	assert(cur->state->layout);

	double lx, ly;
	nx_cursor_absolute_to_layout_coords(cur, dev, x, y, &lx, &ly);
	nx_cursor_warp_closest(cur, dev, lx, ly);
}

void nx_cursor_move(struct nx_cursor *cur, struct nx_input_device *dev,
		double delta_x, double delta_y) {
	assert(cur->state->layout);

	double lx = !isnan(delta_x) ? cur->x + delta_x : cur->x;
	double ly = !isnan(delta_y) ? cur->y + delta_y : cur->y;
	nx_cursor_warp_closest(cur, dev, lx, ly);
}
