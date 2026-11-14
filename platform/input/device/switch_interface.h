#ifndef PLATFORM_INPUT_DEVICE_SWITCH_INTERFACE_H
#define PLATFORM_INPUT_DEVICE_SWITCH_INTERFACE_H

#include "platform/input/device/switch.h"

struct nx_switch_impl {
	const char *name;
};

void nx_switch_init(struct nx_switch *switch_device,
	const struct nx_switch_impl *impl, const char *name);
void nx_switch_finish(struct nx_switch *switch_device);

#endif
