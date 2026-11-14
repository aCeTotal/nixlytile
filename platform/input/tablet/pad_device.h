#ifndef PLATFORM_INPUT_TABLET_PAD_DEVICE_H
#define PLATFORM_INPUT_TABLET_PAD_DEVICE_H

#include <stdint.h>
#include <wayland-server-core.h>
#include "platform/input/device/input_device.h"

struct nx_tablet_pad_impl;

// Multi-mode pads are unsupported.
struct nx_tablet_pad {
	struct nx_input_device base;

	const struct nx_tablet_pad_impl *impl;

	struct {
		struct wl_signal button; // struct nx_tablet_pad_button_event
		struct wl_signal ring; // struct nx_tablet_pad_ring_event
		struct wl_signal strip; // struct nx_tablet_pad_strip_event
	} events;

	size_t button_count;
	size_t ring_count;
	size_t strip_count;

	struct wl_list groups; // nx_tablet_pad_group.link
	struct wl_array paths; // char *
};

struct nx_tablet_pad_group {
	struct wl_list link;

	size_t button_count;
	unsigned int *buttons;

	size_t strip_count;
	unsigned int *strips;

	size_t ring_count;
	unsigned int *rings;

	unsigned int mode_count;
};

struct nx_tablet_pad_button_event {
	uint32_t time_msec;
	uint32_t button;
	enum nx_button_state state;
	unsigned int mode;
	unsigned int group;
};

enum nx_tablet_pad_ring_source {
	NX_TABLET_PAD_RING_SOURCE_UNKNOWN,
	NX_TABLET_PAD_RING_SOURCE_FINGER,
};

struct nx_tablet_pad_ring_event {
	uint32_t time_msec;
	enum nx_tablet_pad_ring_source source;
	uint32_t ring;
	double position;
	unsigned int mode;
};

enum nx_tablet_pad_strip_source {
	NX_TABLET_PAD_STRIP_SOURCE_UNKNOWN,
	NX_TABLET_PAD_STRIP_SOURCE_FINGER,
};

struct nx_tablet_pad_strip_event {
	uint32_t time_msec;
	enum nx_tablet_pad_strip_source source;
	uint32_t strip;
	double position;
	unsigned int mode;
};

// Asserts the device is a tablet pad.
struct nx_tablet_pad *nx_tablet_pad_from_input_device(
	struct nx_input_device *input_device);

#endif
