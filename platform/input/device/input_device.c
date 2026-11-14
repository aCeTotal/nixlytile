#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "platform/input/device/input_device_interface.h"

void nx_input_device_init(struct nx_input_device *dev,
		enum nx_input_device_type type, const char *name) {
	*dev = (struct nx_input_device){
		.type = type,
		.name = name ? strdup(name) : NULL,
	};

	wl_signal_init(&dev->events.destroy);
}

void nx_input_device_finish(struct nx_input_device *dev) {
	wl_signal_emit_mutable(&dev->events.destroy, dev);

	assert(wl_list_empty(&dev->events.destroy.listener_list));

	free(dev->name);
}
