#ifndef PLATFORM_INPUT_TABLET_TOOL_DEVICE_H
#define PLATFORM_INPUT_TABLET_TOOL_DEVICE_H

#include <stdint.h>
#include <wayland-server-core.h>
#include "platform/input/device/input_device.h"

enum nx_tablet_tool_type {
	NX_TABLET_TOOL_TYPE_PEN = 1,
	NX_TABLET_TOOL_TYPE_ERASER,
	NX_TABLET_TOOL_TYPE_BRUSH,
	NX_TABLET_TOOL_TYPE_PENCIL,
	NX_TABLET_TOOL_TYPE_AIRBRUSH,
	NX_TABLET_TOOL_TYPE_MOUSE,
	NX_TABLET_TOOL_TYPE_LENS,
	NX_TABLET_TOOL_TYPE_TOTEM,
};

struct nx_tablet_tool {
	enum nx_tablet_tool_type type;
	uint64_t hardware_serial;
	uint64_t hardware_wacom;

	// Capabilities
	bool tilt;
	bool pressure;
	bool distance;
	bool rotation;
	bool slider;
	bool wheel;

	struct {
		struct wl_signal destroy;
	} events;

	void *data;
};

struct nx_tablet_impl;

struct nx_tablet {
	struct nx_input_device base;

	const struct nx_tablet_impl *impl;

	uint16_t usb_vendor_id, usb_product_id; // zero if unset

	struct {
		struct wl_signal axis; // struct nx_tablet_tool_axis_event
		struct wl_signal proximity; // struct nx_tablet_tool_proximity_event
		struct wl_signal tip; // struct nx_tablet_tool_tip_event
		struct wl_signal button; // struct nx_tablet_tool_button_event
	} events;

	struct wl_array paths; // char *

	void *data;
};

enum nx_tablet_tool_axes {
	NX_TABLET_TOOL_AXIS_X = 1 << 0,
	NX_TABLET_TOOL_AXIS_Y = 1 << 1,
	NX_TABLET_TOOL_AXIS_DISTANCE = 1 << 2,
	NX_TABLET_TOOL_AXIS_PRESSURE = 1 << 3,
	NX_TABLET_TOOL_AXIS_TILT_X = 1 << 4,
	NX_TABLET_TOOL_AXIS_TILT_Y = 1 << 5,
	NX_TABLET_TOOL_AXIS_ROTATION = 1 << 6,
	NX_TABLET_TOOL_AXIS_SLIDER = 1 << 7,
	NX_TABLET_TOOL_AXIS_WHEEL = 1 << 8,
};

struct nx_tablet_tool_axis_event {
	struct nx_tablet *tablet;
	struct nx_tablet_tool *tool;

	uint32_t time_msec;
	uint32_t updated_axes; // enum nx_tablet_tool_axes
	double x, y; // 0..1
	double dx, dy; // since last event
	double pressure;
	double distance;
	double tilt_x, tilt_y;
	double rotation;
	double slider;
	double wheel_delta;
};

enum nx_tablet_tool_proximity_state {
	NX_TABLET_TOOL_PROXIMITY_OUT,
	NX_TABLET_TOOL_PROXIMITY_IN,
};

struct nx_tablet_tool_proximity_event {
	struct nx_tablet *tablet;
	struct nx_tablet_tool *tool;
	uint32_t time_msec;
	double x, y; // 0..1
	enum nx_tablet_tool_proximity_state state;
};

enum nx_tablet_tool_tip_state {
	NX_TABLET_TOOL_TIP_UP,
	NX_TABLET_TOOL_TIP_DOWN,
};

struct nx_tablet_tool_tip_event {
	struct nx_tablet *tablet;
	struct nx_tablet_tool *tool;
	uint32_t time_msec;
	double x, y; // 0..1
	enum nx_tablet_tool_tip_state state;
};

struct nx_tablet_tool_button_event {
	struct nx_tablet *tablet;
	struct nx_tablet_tool *tool;
	uint32_t time_msec;
	uint32_t button;
	enum nx_button_state state;
};

// Asserts the device is a tablet.
struct nx_tablet *nx_tablet_from_input_device(
	struct nx_input_device *input_device);

#endif
