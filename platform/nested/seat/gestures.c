#include "platform/nested/seat/seat.h"

#include "pointer-gestures-unstable-v1-client-protocol.h"

static void gesture_swipe_begin(void *data,
		struct zwp_pointer_gesture_swipe_v1 *zwp_pointer_gesture_swipe_v1,
		uint32_t serial, uint32_t time, struct wl_surface *surface,
		uint32_t fingers) {
	struct nx_wl_pointer *pointer = nested_active_pointer(data);
	if (pointer == NULL) {
		return;
	}
	pointer->fingers = fingers;

	struct nx_pointer_swipe_begin_event event = {
		.pointer = &pointer->nx_pointer,
		.time_msec = time,
		.fingers = fingers,
	};
	wl_signal_emit_mutable(&pointer->nx_pointer.events.swipe_begin, &event);
}

static void gesture_swipe_update(void *data,
		struct zwp_pointer_gesture_swipe_v1 *zwp_pointer_gesture_swipe_v1,
		uint32_t time, wl_fixed_t dx, wl_fixed_t dy) {
	struct nx_wl_pointer *pointer = nested_active_pointer(data);
	if (pointer == NULL) {
		return;
	}

	struct nx_pointer_swipe_update_event event = {
		.pointer = &pointer->nx_pointer,
		.time_msec = time,
		.fingers = pointer->fingers,
		.dx = wl_fixed_to_double(dx),
		.dy = wl_fixed_to_double(dy),
	};
	wl_signal_emit_mutable(&pointer->nx_pointer.events.swipe_update, &event);
}

static void gesture_swipe_end(void *data,
		struct zwp_pointer_gesture_swipe_v1 *zwp_pointer_gesture_swipe_v1,
		uint32_t serial, uint32_t time, int32_t cancelled) {
	struct nx_wl_pointer *pointer = nested_active_pointer(data);
	if (pointer == NULL) {
		return;
	}

	struct nx_pointer_swipe_end_event event = {
		.pointer = &pointer->nx_pointer,
		.time_msec = time,
		.cancelled = cancelled,
	};
	wl_signal_emit_mutable(&pointer->nx_pointer.events.swipe_end, &event);
}

static const struct zwp_pointer_gesture_swipe_v1_listener gesture_swipe_listener = {
	.begin = gesture_swipe_begin,
	.update = gesture_swipe_update,
	.end = gesture_swipe_end,
};

static void gesture_pinch_begin(void *data,
		struct zwp_pointer_gesture_pinch_v1 *zwp_pointer_gesture_pinch_v1,
		uint32_t serial, uint32_t time, struct wl_surface *surface,
		uint32_t fingers) {
	struct nx_wl_pointer *pointer = nested_active_pointer(data);
	if (pointer == NULL) {
		return;
	}
	pointer->fingers = fingers;

	struct nx_pointer_pinch_begin_event event = {
		.pointer = &pointer->nx_pointer,
		.time_msec = time,
		.fingers = pointer->fingers,
	};
	wl_signal_emit_mutable(&pointer->nx_pointer.events.pinch_begin, &event);
}

static void gesture_pinch_update(void *data,
		struct zwp_pointer_gesture_pinch_v1 *zwp_pointer_gesture_pinch_v1,
		uint32_t time, wl_fixed_t dx, wl_fixed_t dy, wl_fixed_t scale,
		wl_fixed_t rotation) {
	struct nx_wl_pointer *pointer = nested_active_pointer(data);
	if (pointer == NULL) {
		return;
	}

	struct nx_pointer_pinch_update_event event = {
		.pointer = &pointer->nx_pointer,
		.time_msec = time,
		.fingers = pointer->fingers,
		.dx = wl_fixed_to_double(dx),
		.dy = wl_fixed_to_double(dy),
		.scale = wl_fixed_to_double(scale),
		.rotation = wl_fixed_to_double(rotation),
	};
	wl_signal_emit_mutable(&pointer->nx_pointer.events.pinch_update, &event);
}

static void gesture_pinch_end(void *data,
		struct zwp_pointer_gesture_pinch_v1 *zwp_pointer_gesture_pinch_v1,
		uint32_t serial, uint32_t time, int32_t cancelled) {
	struct nx_wl_pointer *pointer = nested_active_pointer(data);
	if (pointer == NULL) {
		return;
	}

	struct nx_pointer_pinch_end_event event = {
		.pointer = &pointer->nx_pointer,
		.time_msec = time,
		.cancelled = cancelled,
	};
	wl_signal_emit_mutable(&pointer->nx_pointer.events.pinch_end, &event);
}

static const struct zwp_pointer_gesture_pinch_v1_listener gesture_pinch_listener = {
	.begin = gesture_pinch_begin,
	.update = gesture_pinch_update,
	.end = gesture_pinch_end,
};

static void gesture_hold_begin(void *data,
		struct zwp_pointer_gesture_hold_v1 *zwp_pointer_gesture_hold_v1,
		uint32_t serial, uint32_t time, struct wl_surface *surface,
		uint32_t fingers) {
	struct nx_wl_pointer *pointer = nested_active_pointer(data);
	if (pointer == NULL) {
		return;
	}
	pointer->fingers = fingers;

	struct nx_pointer_hold_begin_event event = {
		.pointer = &pointer->nx_pointer,
		.time_msec = time,
		.fingers = fingers,
	};
	wl_signal_emit_mutable(&pointer->nx_pointer.events.hold_begin, &event);
}

static void gesture_hold_end(void *data,
		struct zwp_pointer_gesture_hold_v1 *zwp_pointer_gesture_hold_v1,
		uint32_t serial, uint32_t time, int32_t cancelled) {
	struct nx_wl_pointer *pointer = nested_active_pointer(data);
	if (pointer == NULL) {
		return;
	}

	struct nx_pointer_hold_end_event event = {
		.pointer = &pointer->nx_pointer,
		.time_msec = time,
		.cancelled = cancelled,
	};
	wl_signal_emit_mutable(&pointer->nx_pointer.events.hold_end, &event);
}

static const struct zwp_pointer_gesture_hold_v1_listener gesture_hold_listener = {
	.begin = gesture_hold_begin,
	.end = gesture_hold_end,
};

void nested_gestures_init(struct nx_wl_seat *seat) {
	struct zwp_pointer_gestures_v1 *gestures = seat->backend->zwp_pointer_gestures_v1;
	if (gestures == NULL) {
		return;
	}

	seat->gesture_swipe = zwp_pointer_gestures_v1_get_swipe_gesture(gestures,
		seat->wl_pointer);
	zwp_pointer_gesture_swipe_v1_add_listener(seat->gesture_swipe,
		&gesture_swipe_listener, seat);

	seat->gesture_pinch = zwp_pointer_gestures_v1_get_pinch_gesture(gestures,
		seat->wl_pointer);
	zwp_pointer_gesture_pinch_v1_add_listener(seat->gesture_pinch,
		&gesture_pinch_listener, seat);

	if (zwp_pointer_gestures_v1_get_version(gestures) <
			ZWP_POINTER_GESTURES_V1_GET_HOLD_GESTURE_SINCE_VERSION) {
		return;
	}
	seat->gesture_hold = zwp_pointer_gestures_v1_get_hold_gesture(gestures,
		seat->wl_pointer);
	zwp_pointer_gesture_hold_v1_add_listener(seat->gesture_hold,
		&gesture_hold_listener, seat);
}

void nested_gestures_finish(struct nx_wl_seat *seat) {
	if (seat->gesture_swipe != NULL) {
		zwp_pointer_gesture_swipe_v1_destroy(seat->gesture_swipe);
	}
	if (seat->gesture_pinch != NULL) {
		zwp_pointer_gesture_pinch_v1_destroy(seat->gesture_pinch);
	}
	if (seat->gesture_hold != NULL) {
		zwp_pointer_gesture_hold_v1_destroy(seat->gesture_hold);
	}
}
