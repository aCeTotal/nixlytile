#ifndef PLATFORM_INPUT_POINTER_VIRTUAL_POINTER_DEVICE_H
#define PLATFORM_INPUT_POINTER_VIRTUAL_POINTER_DEVICE_H

#include "platform/input/pointer/virtual_pointer.h"

// NULL after posting no_memory.
struct nx_virtual_pointer_v1 *input_virtual_pointer_create(
	struct wl_resource *manager_resource, uint32_t id);

#endif
