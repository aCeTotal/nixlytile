#ifndef PLATFORM_INPUT_DEVICE_INPUT_DEVICE_INTERFACE_H
#define PLATFORM_INPUT_DEVICE_INPUT_DEVICE_INTERFACE_H

#include "platform/input/device/input_device.h"

void nx_input_device_init(struct nx_input_device *dev,
	enum nx_input_device_type type, const char *name);
// Emits destroy and frees name.
void nx_input_device_finish(struct nx_input_device *dev);

#endif
