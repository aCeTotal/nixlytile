#include <assert.h>
#include <libinput.h>
#include "platform/input/libinput/libinput_device.h"
#include "platform/input/pointer/pointer_interface.h"
#include "platform/log/log.h"

const struct nx_pointer_impl libinput_pointer_impl = {
	.name = "libinput-pointer",
};

static const struct {
	enum libinput_pointer_axis axis;
	enum wl_pointer_axis orientation;
} scroll_axes[] = {
	{ LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL, WL_POINTER_AXIS_VERTICAL_SCROLL },
	{ LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL, WL_POINTER_AXIS_HORIZONTAL_SCROLL },
};

struct nx_input_device *init_device_pointer(
		struct nx_libinput_input_device *dev) {
	const char *name = get_libinput_device_name(dev->handle);
	nx_pointer_init(&dev->pointer, &libinput_pointer_impl, name);
	return &dev->pointer.base;
}

struct nx_libinput_input_device *device_from_pointer(
		struct nx_pointer *nx_pointer) {
	assert(nx_pointer->impl == &libinput_pointer_impl);

	struct nx_libinput_input_device *dev =
		wl_container_of(nx_pointer, dev, pointer);
	return dev;
}

void handle_pointer_motion(struct libinput_event *event,
		struct nx_pointer *pointer) {
	struct libinput_event_pointer *pevent =
		libinput_event_get_pointer_event(event);
	struct nx_pointer_motion_event nx_event = {
		.pointer = pointer,
		.time_msec = usec_to_msec(libinput_event_pointer_get_time_usec(pevent)),
		.delta_x = libinput_event_pointer_get_dx(pevent),
		.delta_y = libinput_event_pointer_get_dy(pevent),
		.unaccel_dx = libinput_event_pointer_get_dx_unaccelerated(pevent),
		.unaccel_dy = libinput_event_pointer_get_dy_unaccelerated(pevent),
	};
	wl_signal_emit_mutable(&pointer->events.motion, &nx_event);
	wl_signal_emit_mutable(&pointer->events.frame, pointer);
}

void handle_pointer_motion_abs(struct libinput_event *event,
		struct nx_pointer *pointer) {
	struct libinput_event_pointer *pevent =
		libinput_event_get_pointer_event(event);
	struct nx_pointer_motion_absolute_event nx_event = {
		.pointer = pointer,
		.time_msec = usec_to_msec(libinput_event_pointer_get_time_usec(pevent)),
		.x = libinput_event_pointer_get_absolute_x_transformed(pevent, 1),
		.y = libinput_event_pointer_get_absolute_y_transformed(pevent, 1),
	};
	wl_signal_emit_mutable(&pointer->events.motion_absolute, &nx_event);
	wl_signal_emit_mutable(&pointer->events.frame, pointer);
}

static bool pointer_button_state_from_libinput(enum libinput_button_state state,
		enum wl_pointer_button_state *out) {
	switch (state) {
	case LIBINPUT_BUTTON_STATE_PRESSED:
		*out = WL_POINTER_BUTTON_STATE_PRESSED;
		return true;
	case LIBINPUT_BUTTON_STATE_RELEASED:
		*out = WL_POINTER_BUTTON_STATE_RELEASED;
		return true;
	}
	return false;
}

void handle_pointer_button(struct libinput_event *event,
		struct nx_pointer *pointer) {
	struct libinput_event_pointer *pevent =
		libinput_event_get_pointer_event(event);
	struct nx_pointer_button_event nx_event = {
		.pointer = pointer,
		.time_msec = usec_to_msec(libinput_event_pointer_get_time_usec(pevent)),
		.button = libinput_event_pointer_get_button(pevent),
	};
	if (!pointer_button_state_from_libinput(libinput_event_pointer_get_button_state(pevent),
			&nx_event.state)) {
		nx_log(NX_DEBUG, "Unhandled libinput button state");
		return;
	}
	nx_pointer_notify_button(pointer, &nx_event);
	wl_signal_emit_mutable(&pointer->events.frame, pointer);
}

void handle_pointer_axis_value120(struct libinput_event *event,
		struct nx_pointer *pointer, enum wl_pointer_axis_source source) {
	struct libinput_event_pointer *pevent =
		libinput_event_get_pointer_event(event);
	struct nx_pointer_axis_event nx_event = {
		.pointer = pointer,
		.time_msec = usec_to_msec(libinput_event_pointer_get_time_usec(pevent)),
		.source = source,
	};

	for (size_t i = 0; i < sizeof(scroll_axes) / sizeof(scroll_axes[0]); ++i) {
		enum libinput_pointer_axis axis = scroll_axes[i].axis;
		if (!libinput_event_pointer_has_axis(pevent, axis)) {
			continue;
		}
		nx_event.orientation = scroll_axes[i].orientation;
		nx_event.delta = libinput_event_pointer_get_scroll_value(pevent, axis);
		if (source == WL_POINTER_AXIS_SOURCE_WHEEL) {
			nx_event.delta_discrete =
				(int32_t)libinput_event_pointer_get_scroll_value_v120(pevent, axis);
		}
		wl_signal_emit_mutable(&pointer->events.axis, &nx_event);
	}
	wl_signal_emit_mutable(&pointer->events.frame, pointer);
}
