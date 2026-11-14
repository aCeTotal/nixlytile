#include <assert.h>
#include <wayland-server-core.h>
#include "platform/input/seat/seat.h"

void nx_seat_keyboard_start_grab(struct nx_seat *seat,
		struct nx_seat_keyboard_grab *grab) {
	grab->seat = seat;
	seat->keyboard_state.grab = grab;
}

void nx_seat_keyboard_end_grab(struct nx_seat *seat) {
	struct nx_seat_keyboard_grab *grab = seat->keyboard_state.grab;
	if (grab == seat->keyboard_state.default_grab) {
		return;
	}
	seat->keyboard_state.grab = seat->keyboard_state.default_grab;
	if (grab->interface->cancel) {
		grab->interface->cancel(grab);
	}
}

void nx_seat_keyboard_notify_enter(struct nx_seat *seat,
		struct nx_surface *surface, const uint32_t keycodes[], size_t num_keycodes,
		const struct nx_keyboard_modifiers *modifiers) {
	assert(surface);
	struct nx_seat_keyboard_grab *grab = seat->keyboard_state.grab;
	grab->interface->enter(grab, surface, keycodes, num_keycodes, modifiers);
}

void nx_seat_keyboard_notify_clear_focus(struct nx_seat *seat) {
	struct nx_seat_keyboard_grab *grab = seat->keyboard_state.grab;
	grab->interface->clear_focus(grab);
}

void nx_seat_keyboard_notify_key(struct nx_seat *seat, uint32_t time,
		uint32_t key, uint32_t state) {
	struct nx_seat_keyboard_grab *grab = seat->keyboard_state.grab;
	grab->interface->key(grab, time, key, state);
}

void nx_seat_keyboard_notify_modifiers(struct nx_seat *seat,
		const struct nx_keyboard_modifiers *modifiers) {
	struct nx_seat_keyboard_grab *grab = seat->keyboard_state.grab;
	grab->interface->modifiers(grab, modifiers);
}
