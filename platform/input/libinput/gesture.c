#include <libinput.h>
#include "platform/input/libinput/libinput_device.h"

static uint32_t gesture_time_msec(struct libinput_event_gesture *gevent) {
	return usec_to_msec(libinput_event_gesture_get_time_usec(gevent));
}

void handle_pointer_swipe_begin(struct libinput_event *event,
		struct nx_pointer *pointer) {
	struct libinput_event_gesture *gevent =
		libinput_event_get_gesture_event(event);
	struct nx_pointer_swipe_begin_event nx_event = {
		.pointer = pointer,
		.time_msec = gesture_time_msec(gevent),
		.fingers = libinput_event_gesture_get_finger_count(gevent),
	};
	wl_signal_emit_mutable(&pointer->events.swipe_begin, &nx_event);
}

void handle_pointer_swipe_update(struct libinput_event *event,
		struct nx_pointer *pointer) {
	struct libinput_event_gesture *gevent =
		libinput_event_get_gesture_event(event);
	struct nx_pointer_swipe_update_event nx_event = {
		.pointer = pointer,
		.time_msec = gesture_time_msec(gevent),
		.fingers = libinput_event_gesture_get_finger_count(gevent),
		.dx = libinput_event_gesture_get_dx(gevent),
		.dy = libinput_event_gesture_get_dy(gevent),
	};
	wl_signal_emit_mutable(&pointer->events.swipe_update, &nx_event);
}

void handle_pointer_swipe_end(struct libinput_event *event,
		struct nx_pointer *pointer) {
	struct libinput_event_gesture *gevent =
		libinput_event_get_gesture_event(event);
	struct nx_pointer_swipe_end_event nx_event = {
		.pointer = pointer,
		.time_msec = gesture_time_msec(gevent),
		.cancelled = libinput_event_gesture_get_cancelled(gevent),
	};
	wl_signal_emit_mutable(&pointer->events.swipe_end, &nx_event);
}

void handle_pointer_pinch_begin(struct libinput_event *event,
		struct nx_pointer *pointer) {
	struct libinput_event_gesture *gevent =
		libinput_event_get_gesture_event(event);
	struct nx_pointer_pinch_begin_event nx_event = {
		.pointer = pointer,
		.time_msec = gesture_time_msec(gevent),
		.fingers = libinput_event_gesture_get_finger_count(gevent),
	};
	wl_signal_emit_mutable(&pointer->events.pinch_begin, &nx_event);
}

void handle_pointer_pinch_update(struct libinput_event *event,
		struct nx_pointer *pointer) {
	struct libinput_event_gesture *gevent =
		libinput_event_get_gesture_event(event);
	struct nx_pointer_pinch_update_event nx_event = {
		.pointer = pointer,
		.time_msec = gesture_time_msec(gevent),
		.fingers = libinput_event_gesture_get_finger_count(gevent),
		.dx = libinput_event_gesture_get_dx(gevent),
		.dy = libinput_event_gesture_get_dy(gevent),
		.scale = libinput_event_gesture_get_scale(gevent),
		.rotation = libinput_event_gesture_get_angle_delta(gevent),
	};
	wl_signal_emit_mutable(&pointer->events.pinch_update, &nx_event);
}

void handle_pointer_pinch_end(struct libinput_event *event,
		struct nx_pointer *pointer) {
	struct libinput_event_gesture *gevent =
		libinput_event_get_gesture_event(event);
	struct nx_pointer_pinch_end_event nx_event = {
		.pointer = pointer,
		.time_msec = gesture_time_msec(gevent),
		.cancelled = libinput_event_gesture_get_cancelled(gevent),
	};
	wl_signal_emit_mutable(&pointer->events.pinch_end, &nx_event);
}

void handle_pointer_hold_begin(struct libinput_event *event,
		struct nx_pointer *pointer) {
	struct libinput_event_gesture *gevent =
		libinput_event_get_gesture_event(event);
	struct nx_pointer_hold_begin_event nx_event = {
		.pointer = pointer,
		.time_msec = gesture_time_msec(gevent),
		.fingers = libinput_event_gesture_get_finger_count(gevent),
	};
	wl_signal_emit_mutable(&pointer->events.hold_begin, &nx_event);
}

void handle_pointer_hold_end(struct libinput_event *event,
		struct nx_pointer *pointer) {
	struct libinput_event_gesture *gevent =
		libinput_event_get_gesture_event(event);
	struct nx_pointer_hold_end_event nx_event = {
		.pointer = pointer,
		.time_msec = gesture_time_msec(gevent),
		.cancelled = libinput_event_gesture_get_cancelled(gevent),
	};
	wl_signal_emit_mutable(&pointer->events.hold_end, &nx_event);
}
