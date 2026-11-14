#ifndef PLATFORM_INPUT_DEVICE_SWITCH_H
#define PLATFORM_INPUT_DEVICE_SWITCH_H

#include <stdint.h>
#include <wayland-server-core.h>
#include "platform/input/device/input_device.h"

struct nx_switch_impl;

// Lid or tablet-mode switch.
struct nx_switch {
	struct nx_input_device base;

	const struct nx_switch_impl *impl;

	struct {
		struct wl_signal toggle; // struct nx_switch_toggle_event
	} events;
};

enum nx_switch_type {
	NX_SWITCH_TYPE_LID,
	NX_SWITCH_TYPE_TABLET_MODE,
};

enum nx_switch_state {
	NX_SWITCH_STATE_OFF = 0,
	NX_SWITCH_STATE_ON,
};

struct nx_switch_toggle_event {
	uint32_t time_msec;
	enum nx_switch_type switch_type;
	enum nx_switch_state switch_state;
};

// Asserts the device is a switch.
struct nx_switch *nx_switch_from_input_device(
	struct nx_input_device *input_device);

#endif
