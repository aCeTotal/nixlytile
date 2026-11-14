#include <libinput.h>
#include "platform/input/libinput/libinput_device.h"
#include "platform/log/log.h"

static bool proximity_state_from_libinput(
		enum libinput_tablet_tool_proximity_state state,
		enum nx_tablet_tool_proximity_state *out) {
	switch (state) {
	case LIBINPUT_TABLET_TOOL_PROXIMITY_STATE_OUT:
		*out = NX_TABLET_TOOL_PROXIMITY_OUT;
		return true;
	case LIBINPUT_TABLET_TOOL_PROXIMITY_STATE_IN:
		*out = NX_TABLET_TOOL_PROXIMITY_IN;
		return true;
	}
	return false;
}

static bool tip_state_from_libinput(enum libinput_tablet_tool_tip_state state,
		enum nx_tablet_tool_tip_state *out) {
	switch (state) {
	case LIBINPUT_TABLET_TOOL_TIP_UP:
		*out = NX_TABLET_TOOL_TIP_UP;
		return true;
	case LIBINPUT_TABLET_TOOL_TIP_DOWN:
		*out = NX_TABLET_TOOL_TIP_DOWN;
		return true;
	}
	return false;
}

static struct nx_libinput_tablet_tool *event_tool(struct nx_tablet *nx_tablet,
		struct libinput_event_tablet_tool *tevent) {
	struct nx_libinput_input_device *dev = device_from_tablet(nx_tablet);
	return input_libinput_tool_get(dev,
		libinput_event_tablet_tool_get_tool(tevent));
}

void handle_tablet_tool_axis(struct libinput_event *event,
		struct nx_tablet *nx_tablet) {
	struct libinput_event_tablet_tool *tevent =
		libinput_event_get_tablet_tool_event(event);
	struct nx_libinput_tablet_tool *tool = event_tool(nx_tablet, tevent);

	struct nx_tablet_tool_axis_event nx_event = {
		.tablet = nx_tablet,
		.tool = &tool->nx_tool,
		.time_msec = usec_to_msec(libinput_event_tablet_tool_get_time_usec(tevent)),
	};
	if (libinput_event_tablet_tool_x_has_changed(tevent)) {
		nx_event.updated_axes |= NX_TABLET_TOOL_AXIS_X;
		nx_event.x = libinput_event_tablet_tool_get_x_transformed(tevent, 1);
		nx_event.dx = libinput_event_tablet_tool_get_dx(tevent);
	}
	if (libinput_event_tablet_tool_y_has_changed(tevent)) {
		nx_event.updated_axes |= NX_TABLET_TOOL_AXIS_Y;
		nx_event.y = libinput_event_tablet_tool_get_y_transformed(tevent, 1);
		nx_event.dy = libinput_event_tablet_tool_get_dy(tevent);
	}
	if (libinput_event_tablet_tool_pressure_has_changed(tevent)) {
		nx_event.updated_axes |= NX_TABLET_TOOL_AXIS_PRESSURE;
		nx_event.pressure = libinput_event_tablet_tool_get_pressure(tevent);
	}
	if (libinput_event_tablet_tool_distance_has_changed(tevent)) {
		nx_event.updated_axes |= NX_TABLET_TOOL_AXIS_DISTANCE;
		nx_event.distance = libinput_event_tablet_tool_get_distance(tevent);
	}
	if (libinput_event_tablet_tool_tilt_x_has_changed(tevent)) {
		nx_event.updated_axes |= NX_TABLET_TOOL_AXIS_TILT_X;
		nx_event.tilt_x = libinput_event_tablet_tool_get_tilt_x(tevent);
	}
	if (libinput_event_tablet_tool_tilt_y_has_changed(tevent)) {
		nx_event.updated_axes |= NX_TABLET_TOOL_AXIS_TILT_Y;
		nx_event.tilt_y = libinput_event_tablet_tool_get_tilt_y(tevent);
	}
	if (libinput_event_tablet_tool_rotation_has_changed(tevent)) {
		nx_event.updated_axes |= NX_TABLET_TOOL_AXIS_ROTATION;
		nx_event.rotation = libinput_event_tablet_tool_get_rotation(tevent);
	}
	if (libinput_event_tablet_tool_slider_has_changed(tevent)) {
		nx_event.updated_axes |= NX_TABLET_TOOL_AXIS_SLIDER;
		nx_event.slider = libinput_event_tablet_tool_get_slider_position(tevent);
	}
	if (libinput_event_tablet_tool_wheel_has_changed(tevent)) {
		nx_event.updated_axes |= NX_TABLET_TOOL_AXIS_WHEEL;
		nx_event.wheel_delta = libinput_event_tablet_tool_get_wheel_delta(tevent);
	}
	wl_signal_emit_mutable(&nx_tablet->events.axis, &nx_event);
}

void handle_tablet_tool_proximity(struct libinput_event *event,
		struct nx_tablet *nx_tablet) {
	struct libinput_event_tablet_tool *tevent =
		libinput_event_get_tablet_tool_event(event);
	struct nx_libinput_tablet_tool *tool = event_tool(nx_tablet, tevent);

	struct nx_tablet_tool_proximity_event nx_event = {
		.tablet = nx_tablet,
		.tool = &tool->nx_tool,
		.time_msec = usec_to_msec(libinput_event_tablet_tool_get_time_usec(tevent)),
		.x = libinput_event_tablet_tool_get_x_transformed(tevent, 1),
		.y = libinput_event_tablet_tool_get_y_transformed(tevent, 1),
	};

	if (!proximity_state_from_libinput(libinput_event_tablet_tool_get_proximity_state(tevent),
			&nx_event.state)) {
		nx_log(NX_DEBUG, "Unhandled libinput tablet tool proximity state");
		return;
	}

	wl_signal_emit_mutable(&nx_tablet->events.proximity, &nx_event);

	if (libinput_event_tablet_tool_get_proximity_state(tevent) ==
			LIBINPUT_TABLET_TOOL_PROXIMITY_STATE_IN) {
		handle_tablet_tool_axis(event, nx_tablet);
	}

	// libinput forgets non-unique tools.
	if (!libinput_tablet_tool_is_unique(tool->handle)
			&& libinput_event_tablet_tool_get_proximity_state(tevent) ==
			LIBINPUT_TABLET_TOOL_PROXIMITY_STATE_OUT) {
		input_libinput_tool_destroy(tool);
	}
}

void handle_tablet_tool_tip(struct libinput_event *event,
		struct nx_tablet *nx_tablet) {
	handle_tablet_tool_axis(event, nx_tablet);
	struct libinput_event_tablet_tool *tevent =
		libinput_event_get_tablet_tool_event(event);
	struct nx_libinput_tablet_tool *tool = event_tool(nx_tablet, tevent);

	struct nx_tablet_tool_tip_event nx_event = {
		.tablet = nx_tablet,
		.tool = &tool->nx_tool,
		.time_msec = usec_to_msec(libinput_event_tablet_tool_get_time_usec(tevent)),
		.x = libinput_event_tablet_tool_get_x_transformed(tevent, 1),
		.y = libinput_event_tablet_tool_get_y_transformed(tevent, 1),
	};

	if (!tip_state_from_libinput(libinput_event_tablet_tool_get_tip_state(tevent),
			&nx_event.state)) {
		nx_log(NX_DEBUG, "Unhandled libinput tablet tool tip state");
		return;
	}

	wl_signal_emit_mutable(&nx_tablet->events.tip, &nx_event);
}

void handle_tablet_tool_button(struct libinput_event *event,
		struct nx_tablet *nx_tablet) {
	handle_tablet_tool_axis(event, nx_tablet);
	struct libinput_event_tablet_tool *tevent =
		libinput_event_get_tablet_tool_event(event);
	struct nx_libinput_tablet_tool *tool = event_tool(nx_tablet, tevent);

	struct nx_tablet_tool_button_event nx_event = {
		.tablet = nx_tablet,
		.tool = &tool->nx_tool,
		.time_msec = usec_to_msec(libinput_event_tablet_tool_get_time_usec(tevent)),
		.button = libinput_event_tablet_tool_get_button(tevent),
	};

	if (!button_state_from_libinput(libinput_event_tablet_tool_get_button_state(tevent),
			&nx_event.state)) {
		nx_log(NX_DEBUG, "Unhandled libinput button state");
		return;
	}

	wl_signal_emit_mutable(&nx_tablet->events.button, &nx_event);
}
