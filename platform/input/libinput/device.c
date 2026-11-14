#include <assert.h>
#include <libinput.h>
#include <stdlib.h>
#include "platform/input/device/switch_interface.h"
#include "platform/input/keyboard/keyboard_interface.h"
#include "platform/input/libinput/libinput_device.h"
#include "platform/input/pointer/pointer_interface.h"

void destroy_libinput_input_device(struct nx_libinput_input_device *dev) {
	if (dev->keyboard.impl) {
		nx_keyboard_finish(&dev->keyboard);
	}
	if (dev->pointer.impl) {
		nx_pointer_finish(&dev->pointer);
	}
	if (dev->switch_device.impl) {
		nx_switch_finish(&dev->switch_device);
	}
	if (dev->tablet.impl) {
		finish_device_tablet(dev);
	}
	if (dev->tablet_pad.impl) {
		finish_device_tablet_pad(dev);
	}

	libinput_device_unref(dev->handle);
	wl_list_remove(&dev->link);
	free(dev);
}

bool nx_input_device_is_libinput(struct nx_input_device *nx_dev) {
	switch (nx_dev->type) {
	case NX_INPUT_DEVICE_KEYBOARD:
		return nx_keyboard_from_input_device(nx_dev)->impl ==
			&libinput_keyboard_impl;
	case NX_INPUT_DEVICE_POINTER:
		return nx_pointer_from_input_device(nx_dev)->impl ==
			&libinput_pointer_impl;
	case NX_INPUT_DEVICE_TABLET:
		return nx_tablet_from_input_device(nx_dev)->impl ==
			&libinput_tablet_impl;
	case NX_INPUT_DEVICE_TABLET_PAD:
		return nx_tablet_pad_from_input_device(nx_dev)->impl ==
			&libinput_tablet_pad_impl;
	case NX_INPUT_DEVICE_SWITCH:
		return nx_switch_from_input_device(nx_dev)->impl ==
			&libinput_switch_impl;
	default:
		return false;
	}
}

struct libinput_device *nx_libinput_get_device_handle(
		struct nx_input_device *nx_dev) {
	struct nx_libinput_input_device *dev = NULL;
	switch (nx_dev->type) {
	case NX_INPUT_DEVICE_KEYBOARD:
		dev = device_from_keyboard(nx_keyboard_from_input_device(nx_dev));
		break;
	case NX_INPUT_DEVICE_POINTER:
		dev = device_from_pointer(nx_pointer_from_input_device(nx_dev));
		break;
	case NX_INPUT_DEVICE_SWITCH:
		dev = device_from_switch(nx_switch_from_input_device(nx_dev));
		break;
	case NX_INPUT_DEVICE_TABLET:
		dev = device_from_tablet(nx_tablet_from_input_device(nx_dev));
		break;
	case NX_INPUT_DEVICE_TABLET_PAD:
		dev = device_from_tablet_pad(nx_tablet_pad_from_input_device(nx_dev));
		break;
	}

	assert(dev);
	return dev->handle;
}

const char *get_libinput_device_name(struct libinput_device *device) {
	const char *name = libinput_device_get_name(device);
	if (name[0] == '\0') {
		return NULL;
	}
	return name;
}
