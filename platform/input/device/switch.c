#include <assert.h>
#include <wayland-server-core.h>
#include "platform/input/device/input_device_interface.h"
#include "platform/input/device/switch_interface.h"

struct nx_switch *nx_switch_from_input_device(
		struct nx_input_device *input_device) {
	assert(input_device->type == NX_INPUT_DEVICE_SWITCH);
	return wl_container_of(input_device, (struct nx_switch *)NULL, base);
}

void nx_switch_init(struct nx_switch *switch_device,
		const struct nx_switch_impl *impl, const char *name) {
	*switch_device = (struct nx_switch){
		.impl = impl,
	};
	nx_input_device_init(&switch_device->base, NX_INPUT_DEVICE_SWITCH, name);

	wl_signal_init(&switch_device->events.toggle);
}

void nx_switch_finish(struct nx_switch *switch_device) {
	nx_input_device_finish(&switch_device->base);

	assert(wl_list_empty(&switch_device->events.toggle.listener_list));
}
