#include "platform/input/pointer/pointer_interface.h"
#include "platform/log/log.h"
#include "platform/nested/output/output.h"
#include "platform/nested/seat/seat.h"

#include "relative-pointer-unstable-v1-client-protocol.h"

static void pointer_handle_enter(void *data, struct wl_pointer *wl_pointer,
		uint32_t serial, struct wl_surface *surface, wl_fixed_t sx,
		wl_fixed_t sy) {
	struct nx_wl_seat *seat = data;
	if (surface == NULL) {
		return;
	}
	struct nx_wl_output *output = nested_output_from_surface(seat->backend, surface);
	if (output == NULL) {
		return;
	}

	struct nx_wl_pointer *pointer = nested_pointer_for_output(output, wl_pointer);
	seat->active_pointer = pointer;

	// One seat owns the output cursor.
	struct nx_wl_pointer *current = output->cursor.pointer;
	if (current && current != pointer) {
		nx_log(NX_INFO, "Ignoring seat '%s' pointer in favor of seat '%s'",
			seat->name, current->seat->name);
		return;
	}

	output->enter_serial = serial;
	output->cursor.pointer = pointer;
	nested_output_update_cursor(output);
}

static void pointer_handle_leave(void *data, struct wl_pointer *wl_pointer,
		uint32_t serial, struct wl_surface *surface) {
	struct nx_wl_seat *seat = data;
	if (surface == NULL) {
		return;
	}
	struct nx_wl_output *output = nested_output_from_surface(seat->backend, surface);
	if (output == NULL) {
		return;
	}

	if (seat->active_pointer != NULL && seat->active_pointer->output == output) {
		seat->active_pointer = NULL;
	}
	if (output->cursor.pointer == seat->active_pointer) {
		output->enter_serial = 0;
		output->cursor.pointer = NULL;
	}
}

static void pointer_handle_motion(void *data, struct wl_pointer *wl_pointer,
		uint32_t time, wl_fixed_t sx, wl_fixed_t sy) {
	struct nx_wl_pointer *pointer = nested_active_pointer(data);
	if (pointer == NULL) {
		return;
	}

	struct nx_output *nx_output = &pointer->output->nx_output;
	struct nx_pointer_motion_absolute_event event = {
		.pointer = &pointer->nx_pointer,
		.time_msec = time,
		.x = wl_fixed_to_double(sx) / nx_output->width,
		.y = wl_fixed_to_double(sy) / nx_output->height,
	};
	wl_signal_emit_mutable(&pointer->nx_pointer.events.motion_absolute, &event);
}

static void pointer_handle_button(void *data, struct wl_pointer *wl_pointer,
		uint32_t serial, uint32_t time, uint32_t button, uint32_t state) {
	struct nx_wl_pointer *pointer = nested_active_pointer(data);
	if (pointer == NULL) {
		return;
	}

	struct nx_pointer_button_event event = {
		.pointer = &pointer->nx_pointer,
		.button = button,
		.state = state,
		.time_msec = time,
	};
	nx_pointer_notify_button(&pointer->nx_pointer, &event);
}

static void pointer_handle_axis(void *data, struct wl_pointer *wl_pointer,
		uint32_t time, uint32_t axis, wl_fixed_t value) {
	struct nx_wl_pointer *pointer = nested_active_pointer(data);
	if (pointer == NULL) {
		return;
	}

	struct nx_pointer_axis_event event = {
		.pointer = &pointer->nx_pointer,
		.delta = wl_fixed_to_double(value),
		.delta_discrete = pointer->axis_discrete,
		.orientation = axis,
		.time_msec = time,
		.source = pointer->axis_source,
		.relative_direction = pointer->axis_relative_direction,
	};
	wl_signal_emit_mutable(&pointer->nx_pointer.events.axis, &event);

	pointer->axis_discrete = 0;
}

static void pointer_handle_frame(void *data, struct wl_pointer *wl_pointer) {
	struct nx_wl_pointer *pointer = nested_active_pointer(data);
	if (pointer == NULL) {
		return;
	}
	wl_signal_emit_mutable(&pointer->nx_pointer.events.frame, &pointer->nx_pointer);
}

static void pointer_handle_axis_source(void *data,
		struct wl_pointer *wl_pointer, uint32_t axis_source) {
	struct nx_wl_pointer *pointer = nested_active_pointer(data);
	if (pointer == NULL) {
		return;
	}
	pointer->axis_source = axis_source;
}

static void pointer_handle_axis_stop(void *data, struct wl_pointer *wl_pointer,
		uint32_t time, uint32_t axis) {
	struct nx_wl_pointer *pointer = nested_active_pointer(data);
	if (pointer == NULL) {
		return;
	}

	struct nx_pointer_axis_event event = {
		.pointer = &pointer->nx_pointer,
		.delta = 0,
		.delta_discrete = 0,
		.orientation = axis,
		.time_msec = time,
		.source = pointer->axis_source,
	};
	wl_signal_emit_mutable(&pointer->nx_pointer.events.axis, &event);
}

static void pointer_handle_axis_discrete(void *data,
		struct wl_pointer *wl_pointer, uint32_t axis, int32_t discrete) {
	struct nx_wl_pointer *pointer = nested_active_pointer(data);
	if (pointer == NULL) {
		return;
	}
	pointer->axis_discrete = discrete * NX_POINTER_AXIS_DISCRETE_STEP;
}

static void pointer_handle_axis_value120(void *data,
		struct wl_pointer *wl_pointer, uint32_t axis, int32_t value120) {
	struct nx_wl_pointer *pointer = nested_active_pointer(data);
	if (pointer == NULL) {
		return;
	}
	pointer->axis_discrete = value120;
}

static void pointer_handle_axis_relative_direction(void *data,
		struct wl_pointer *wl_pointer, uint32_t axis, uint32_t direction) {
	struct nx_wl_pointer *pointer = nested_active_pointer(data);
	if (pointer == NULL) {
		return;
	}
	pointer->axis_relative_direction = direction;
}

const struct wl_pointer_listener nested_pointer_listener = {
	.enter = pointer_handle_enter,
	.leave = pointer_handle_leave,
	.motion = pointer_handle_motion,
	.button = pointer_handle_button,
	.axis = pointer_handle_axis,
	.frame = pointer_handle_frame,
	.axis_source = pointer_handle_axis_source,
	.axis_stop = pointer_handle_axis_stop,
	.axis_discrete = pointer_handle_axis_discrete,
	.axis_value120 = pointer_handle_axis_value120,
	.axis_relative_direction = pointer_handle_axis_relative_direction,
};

static void relative_pointer_handle_relative_motion(void *data,
		struct zwp_relative_pointer_v1 *relative_pointer, uint32_t utime_hi,
		uint32_t utime_lo, wl_fixed_t dx, wl_fixed_t dy, wl_fixed_t dx_unaccel,
		wl_fixed_t dy_unaccel) {
	struct nx_wl_pointer *pointer = nested_active_pointer(data);
	if (pointer == NULL) {
		return;
	}

	uint64_t time_usec = (uint64_t)utime_hi << 32 | utime_lo;
	struct nx_pointer_motion_event event = {
		.pointer = &pointer->nx_pointer,
		.time_msec = (uint32_t)(time_usec / 1000),
		.delta_x = wl_fixed_to_double(dx),
		.delta_y = wl_fixed_to_double(dy),
		.unaccel_dx = wl_fixed_to_double(dx_unaccel),
		.unaccel_dy = wl_fixed_to_double(dy_unaccel),
	};
	wl_signal_emit_mutable(&pointer->nx_pointer.events.motion, &event);
}

const struct zwp_relative_pointer_v1_listener nested_relative_pointer_listener = {
	.relative_motion = relative_pointer_handle_relative_motion,
};
