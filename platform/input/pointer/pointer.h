#ifndef PLATFORM_INPUT_POINTER_POINTER_H
#define PLATFORM_INPUT_POINTER_POINTER_H

#include <stdint.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include "platform/input/device/input_device.h"

#define NX_POINTER_BUTTONS_CAP 16

struct nx_pointer_impl;

struct nx_pointer {
	struct nx_input_device base;

	const struct nx_pointer_impl *impl;

	char *output_name;

	uint32_t buttons[NX_POINTER_BUTTONS_CAP];
	size_t button_count;

	struct {
		struct wl_signal motion; // struct nx_pointer_motion_event
		struct wl_signal motion_absolute; // struct nx_pointer_motion_absolute_event
		struct wl_signal button; // struct nx_pointer_button_event
		struct wl_signal axis; // struct nx_pointer_axis_event
		struct wl_signal frame;

		struct wl_signal swipe_begin; // struct nx_pointer_swipe_begin_event
		struct wl_signal swipe_update; // struct nx_pointer_swipe_update_event
		struct wl_signal swipe_end; // struct nx_pointer_swipe_end_event

		struct wl_signal pinch_begin; // struct nx_pointer_pinch_begin_event
		struct wl_signal pinch_update; // struct nx_pointer_pinch_update_event
		struct wl_signal pinch_end; // struct nx_pointer_pinch_end_event

		struct wl_signal hold_begin; // struct nx_pointer_hold_begin_event
		struct wl_signal hold_end; // struct nx_pointer_hold_end_event
	} events;
};

struct nx_pointer_motion_event {
	struct nx_pointer *pointer;
	uint32_t time_msec;
	double delta_x, delta_y;
	double unaccel_dx, unaccel_dy;
};

struct nx_pointer_motion_absolute_event {
	struct nx_pointer *pointer;
	uint32_t time_msec;
	double x, y; // 0..1
};

struct nx_pointer_button_event {
	struct nx_pointer *pointer;
	uint32_t time_msec;
	uint32_t button;
	enum wl_pointer_button_state state;
};

#define NX_POINTER_AXIS_DISCRETE_STEP 120

struct nx_pointer_axis_event {
	struct nx_pointer *pointer;
	uint32_t time_msec;
	enum wl_pointer_axis_source source;
	enum wl_pointer_axis orientation;
	enum wl_pointer_axis_relative_direction relative_direction;
	double delta;
	int32_t delta_discrete;
};

struct nx_pointer_swipe_begin_event {
	struct nx_pointer *pointer;
	uint32_t time_msec;
	uint32_t fingers;
};

struct nx_pointer_swipe_update_event {
	struct nx_pointer *pointer;
	uint32_t time_msec;
	uint32_t fingers;
	double dx, dy; // centre delta since last event
};

struct nx_pointer_swipe_end_event {
	struct nx_pointer *pointer;
	uint32_t time_msec;
	bool cancelled;
};

struct nx_pointer_pinch_begin_event {
	struct nx_pointer *pointer;
	uint32_t time_msec;
	uint32_t fingers;
};

struct nx_pointer_pinch_update_event {
	struct nx_pointer *pointer;
	uint32_t time_msec;
	uint32_t fingers;
	double dx, dy; // centre delta since last event
	double scale; // relative to the begin event
	double rotation; // clockwise degrees since last event
};

struct nx_pointer_pinch_end_event {
	struct nx_pointer *pointer;
	uint32_t time_msec;
	bool cancelled;
};

struct nx_pointer_hold_begin_event {
	struct nx_pointer *pointer;
	uint32_t time_msec;
	uint32_t fingers;
};

struct nx_pointer_hold_end_event {
	struct nx_pointer *pointer;
	uint32_t time_msec;
	bool cancelled;
};

// Asserts the device is a pointer.
struct nx_pointer *nx_pointer_from_input_device(
	struct nx_input_device *input_device);

#endif
