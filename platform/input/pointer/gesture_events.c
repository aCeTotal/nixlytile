#include <wayland-server-core.h>
#include "platform/input/pointer/gestures.h"
#include "platform/surface/compositor.h"
#include "pointer-gestures-unstable-v1-protocol.h"

// Client of the focused surface.
static struct nx_seat_client *focused_client(struct nx_seat *seat) {
	if (seat->pointer_state.focused_surface == NULL) {
		return NULL;
	}
	return seat->pointer_state.focused_client;
}

// Gesture user data is its seat.
static bool gesture_targets(struct wl_resource *gesture, struct nx_seat *seat,
		struct nx_seat_client *focus) {
	return wl_resource_get_user_data(gesture) == seat &&
		wl_resource_get_client(gesture) == focus->client;
}

void nx_pointer_gestures_v1_send_swipe_begin(
		struct nx_pointer_gestures_v1 *gestures, struct nx_seat *seat,
		uint32_t time_msec, uint32_t fingers) {
	struct nx_seat_client *focus = focused_client(seat);
	if (focus == NULL) {
		return;
	}
	uint32_t serial = nx_seat_client_next_serial(focus);
	struct wl_resource *surface = seat->pointer_state.focused_surface->resource;

	struct wl_resource *gesture;
	wl_resource_for_each(gesture, &gestures->swipes) {
		if (gesture_targets(gesture, seat, focus)) {
			zwp_pointer_gesture_swipe_v1_send_begin(gesture, serial,
				time_msec, surface, fingers);
		}
	}
}

void nx_pointer_gestures_v1_send_swipe_update(
		struct nx_pointer_gestures_v1 *gestures, struct nx_seat *seat,
		uint32_t time_msec, double dx, double dy) {
	struct nx_seat_client *focus = focused_client(seat);
	if (focus == NULL) {
		return;
	}

	struct wl_resource *gesture;
	wl_resource_for_each(gesture, &gestures->swipes) {
		if (gesture_targets(gesture, seat, focus)) {
			zwp_pointer_gesture_swipe_v1_send_update(gesture, time_msec,
				wl_fixed_from_double(dx), wl_fixed_from_double(dy));
		}
	}
}

void nx_pointer_gestures_v1_send_swipe_end(
		struct nx_pointer_gestures_v1 *gestures, struct nx_seat *seat,
		uint32_t time_msec, bool cancelled) {
	struct nx_seat_client *focus = focused_client(seat);
	if (focus == NULL) {
		return;
	}
	uint32_t serial = nx_seat_client_next_serial(focus);

	struct wl_resource *gesture;
	wl_resource_for_each(gesture, &gestures->swipes) {
		if (gesture_targets(gesture, seat, focus)) {
			zwp_pointer_gesture_swipe_v1_send_end(gesture, serial,
				time_msec, cancelled);
		}
	}
}

void nx_pointer_gestures_v1_send_pinch_begin(
		struct nx_pointer_gestures_v1 *gestures, struct nx_seat *seat,
		uint32_t time_msec, uint32_t fingers) {
	struct nx_seat_client *focus = focused_client(seat);
	if (focus == NULL) {
		return;
	}
	uint32_t serial = nx_seat_client_next_serial(focus);
	struct wl_resource *surface = seat->pointer_state.focused_surface->resource;

	struct wl_resource *gesture;
	wl_resource_for_each(gesture, &gestures->pinches) {
		if (gesture_targets(gesture, seat, focus)) {
			zwp_pointer_gesture_pinch_v1_send_begin(gesture, serial,
				time_msec, surface, fingers);
		}
	}
}

void nx_pointer_gestures_v1_send_pinch_update(
		struct nx_pointer_gestures_v1 *gestures, struct nx_seat *seat,
		uint32_t time_msec, double dx, double dy, double scale,
		double rotation) {
	struct nx_seat_client *focus = focused_client(seat);
	if (focus == NULL) {
		return;
	}

	struct wl_resource *gesture;
	wl_resource_for_each(gesture, &gestures->pinches) {
		if (gesture_targets(gesture, seat, focus)) {
			zwp_pointer_gesture_pinch_v1_send_update(gesture, time_msec,
				wl_fixed_from_double(dx), wl_fixed_from_double(dy),
				wl_fixed_from_double(scale), wl_fixed_from_double(rotation));
		}
	}
}

void nx_pointer_gestures_v1_send_pinch_end(
		struct nx_pointer_gestures_v1 *gestures, struct nx_seat *seat,
		uint32_t time_msec, bool cancelled) {
	struct nx_seat_client *focus = focused_client(seat);
	if (focus == NULL) {
		return;
	}
	uint32_t serial = nx_seat_client_next_serial(focus);

	struct wl_resource *gesture;
	wl_resource_for_each(gesture, &gestures->pinches) {
		if (gesture_targets(gesture, seat, focus)) {
			zwp_pointer_gesture_pinch_v1_send_end(gesture, serial,
				time_msec, cancelled);
		}
	}
}

void nx_pointer_gestures_v1_send_hold_begin(
		struct nx_pointer_gestures_v1 *gestures, struct nx_seat *seat,
		uint32_t time_msec, uint32_t fingers) {
	struct nx_seat_client *focus = focused_client(seat);
	if (focus == NULL) {
		return;
	}
	uint32_t serial = nx_seat_client_next_serial(focus);
	struct wl_resource *surface = seat->pointer_state.focused_surface->resource;

	struct wl_resource *gesture;
	wl_resource_for_each(gesture, &gestures->holds) {
		if (gesture_targets(gesture, seat, focus)) {
			zwp_pointer_gesture_hold_v1_send_begin(gesture, serial,
				time_msec, surface, fingers);
		}
	}
}

void nx_pointer_gestures_v1_send_hold_end(
		struct nx_pointer_gestures_v1 *gestures, struct nx_seat *seat,
		uint32_t time_msec, bool cancelled) {
	struct nx_seat_client *focus = focused_client(seat);
	if (focus == NULL) {
		return;
	}
	uint32_t serial = nx_seat_client_next_serial(focus);

	struct wl_resource *gesture;
	wl_resource_for_each(gesture, &gestures->holds) {
		if (gesture_targets(gesture, seat, focus)) {
			zwp_pointer_gesture_hold_v1_send_end(gesture, serial,
				time_msec, cancelled);
		}
	}
}
