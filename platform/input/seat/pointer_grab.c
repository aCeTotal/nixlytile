#include <assert.h>
#include <wayland-server-core.h>
#include "platform/input/seat/seat.h"
#include "platform/log/log.h"

void nx_seat_pointer_start_grab(struct nx_seat *seat,
		struct nx_seat_pointer_grab *grab) {
	assert(seat);
	grab->seat = seat;
	seat->pointer_state.grab = grab;
}

void nx_seat_pointer_end_grab(struct nx_seat *seat) {
	struct nx_seat_pointer_grab *grab = seat->pointer_state.grab;
	if (grab == seat->pointer_state.default_grab) {
		return;
	}
	seat->pointer_state.grab = seat->pointer_state.default_grab;
	if (grab->interface->cancel) {
		grab->interface->cancel(grab);
	}
}

// New focus knows no buttons.
static void reset_buttons_on_focus_change(struct nx_seat *seat,
		struct nx_surface *old_focus) {
	if (old_focus != seat->pointer_state.focused_surface) {
		seat->pointer_state.button_count = 0;
	}
}

void nx_seat_pointer_notify_enter(struct nx_seat *seat,
		struct nx_surface *surface, double sx, double sy) {
	assert(surface);
	struct nx_seat_pointer_grab *grab = seat->pointer_state.grab;
	struct nx_surface *old_focus = seat->pointer_state.focused_surface;
	grab->interface->enter(grab, surface, sx, sy);
	reset_buttons_on_focus_change(seat, old_focus);
}

void nx_seat_pointer_notify_clear_focus(struct nx_seat *seat) {
	struct nx_seat_pointer_grab *grab = seat->pointer_state.grab;
	struct nx_surface *old_focus = seat->pointer_state.focused_surface;
	grab->interface->clear_focus(grab);
	reset_buttons_on_focus_change(seat, old_focus);
}

void nx_seat_pointer_notify_motion(struct nx_seat *seat, uint32_t time,
		double sx, double sy) {
	struct nx_seat_pointer_grab *grab = seat->pointer_state.grab;
	grab->interface->motion(grab, time, sx, sy);
}

static struct nx_seat_pointer_button *find_button(
		struct nx_seat_pointer_state *state, uint32_t button) {
	for (size_t i = 0; i < state->button_count; i++) {
		if (state->buttons[i].button == button) {
			return &state->buttons[i];
		}
	}
	return NULL;
}

// Forwards only the first press.
static bool track_press(struct nx_seat_pointer_state *state, uint32_t button) {
	if (state->button_count == 0) {
		state->grab_button = button;
	}
	struct nx_seat_pointer_button *pressed = find_button(state, button);
	if (pressed != NULL) {
		pressed->n_pressed++;
		return false;
	}
	if (state->button_count == NX_POINTER_BUTTONS_CAP) {
		return false;
	}
	state->buttons[state->button_count++] = (struct nx_seat_pointer_button){
		.button = button,
		.n_pressed = 1,
	};
	return true;
}

// Forwards only the last release.
static bool track_release(struct nx_seat_pointer_state *state, uint32_t button) {
	struct nx_seat_pointer_button *pressed = find_button(state, button);
	if (pressed == NULL || --pressed->n_pressed > 0) {
		return false;
	}
	*pressed = state->buttons[--state->button_count];
	return true;
}

uint32_t nx_seat_pointer_notify_button(struct nx_seat *seat,
		uint32_t time, uint32_t button, enum wl_pointer_button_state state) {
	struct nx_seat_pointer_state *pointer = &seat->pointer_state;
	bool pressed = state == WL_POINTER_BUTTON_STATE_PRESSED;
	bool forward = pressed ? track_press(pointer, button) :
		track_release(pointer, button);
	if (!forward) {
		return 0;
	}

	struct nx_seat_pointer_grab *grab = pointer->grab;
	uint32_t serial = grab->interface->button(grab, time, button, state);
	if (serial && pointer->button_count == 1 && pressed) {
		pointer->grab_serial = serial;
	}
	return serial;
}

void nx_seat_pointer_notify_axis(struct nx_seat *seat, uint32_t time,
		enum wl_pointer_axis orientation, double value,
		int32_t value_discrete, enum wl_pointer_axis_source source,
		enum wl_pointer_axis_relative_direction relative_direction) {
	struct nx_seat_pointer_grab *grab = seat->pointer_state.grab;
	grab->interface->axis(grab, time, orientation, value, value_discrete,
		source, relative_direction);
}

void nx_seat_pointer_notify_frame(struct nx_seat *seat) {
	struct nx_seat_pointer_grab *grab = seat->pointer_state.grab;
	if (grab->interface->frame) {
		grab->interface->frame(grab);
	}
}

bool nx_seat_validate_pointer_grab_serial(struct nx_seat *seat,
		struct nx_surface *origin, uint32_t serial) {
	struct nx_seat_pointer_state *state = &seat->pointer_state;
	if (state->button_count != 1 || state->grab_serial != serial) {
		nx_log(NX_DEBUG, "Pointer grab serial validation failed: "
			"button_count=%zu grab_serial=%"PRIu32" (got %"PRIu32")",
			state->button_count, state->grab_serial, serial);
		return false;
	}
	if (origin != NULL && state->focused_surface != origin) {
		nx_log(NX_DEBUG, "Pointer grab serial validation failed: "
			"invalid origin surface");
		return false;
	}
	return true;
}
