#include <assert.h>
#include <stdlib.h>
#include <wayland-server-core.h>
#include "platform/input/device/input_device_interface.h"
#include "platform/input/tablet/tool_interface.h"

struct nx_tablet *nx_tablet_from_input_device(
		struct nx_input_device *input_device) {
	assert(input_device->type == NX_INPUT_DEVICE_TABLET);
	return wl_container_of(input_device, (struct nx_tablet *)NULL, base);
}

void nx_tablet_init(struct nx_tablet *tablet,
		const struct nx_tablet_impl *impl, const char *name) {
	*tablet = (struct nx_tablet){
		.impl = impl,
	};
	nx_input_device_init(&tablet->base, NX_INPUT_DEVICE_TABLET, name);

	wl_signal_init(&tablet->events.axis);
	wl_signal_init(&tablet->events.proximity);
	wl_signal_init(&tablet->events.tip);
	wl_signal_init(&tablet->events.button);

	wl_array_init(&tablet->paths);
}

void nx_tablet_finish(struct nx_tablet *tablet) {
	nx_input_device_finish(&tablet->base);

	assert(wl_list_empty(&tablet->events.axis.listener_list));
	assert(wl_list_empty(&tablet->events.proximity.listener_list));
	assert(wl_list_empty(&tablet->events.tip.listener_list));
	assert(wl_list_empty(&tablet->events.button.listener_list));

	char **path_ptr;
	wl_array_for_each(path_ptr, &tablet->paths) {
		free(*path_ptr);
	}
	wl_array_release(&tablet->paths);
}
