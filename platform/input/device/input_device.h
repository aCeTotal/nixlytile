#ifndef PLATFORM_INPUT_DEVICE_INPUT_DEVICE_H
#define PLATFORM_INPUT_DEVICE_INPUT_DEVICE_H

#include <wayland-server-core.h>

enum nx_button_state {
	NX_BUTTON_RELEASED,
	NX_BUTTON_PRESSED,
};

enum nx_input_device_type {
	NX_INPUT_DEVICE_KEYBOARD, // struct nx_keyboard
	NX_INPUT_DEVICE_POINTER, // struct nx_pointer
	NX_INPUT_DEVICE_TABLET, // struct nx_tablet
	NX_INPUT_DEVICE_TABLET_PAD, // struct nx_tablet_pad
	NX_INPUT_DEVICE_SWITCH, // struct nx_switch
};

// Base of every typed input device.
struct nx_input_device {
	enum nx_input_device_type type;
	char *name; // may be NULL

	struct {
		struct wl_signal destroy;
	} events;
};

#endif
