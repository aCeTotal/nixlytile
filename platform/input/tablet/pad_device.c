#include <assert.h>
#include <stdlib.h>
#include <wayland-server-core.h>
#include "platform/input/device/input_device_interface.h"
#include "platform/input/tablet/pad_interface.h"
#include "platform/log/log.h"

struct nx_tablet_pad *nx_tablet_pad_from_input_device(
		struct nx_input_device *input_device) {
	assert(input_device->type == NX_INPUT_DEVICE_TABLET_PAD);
	return wl_container_of(input_device, (struct nx_tablet_pad *)NULL, base);
}

void nx_tablet_pad_init(struct nx_tablet_pad *pad,
		const struct nx_tablet_pad_impl *impl, const char *name) {
	*pad = (struct nx_tablet_pad){
		.impl = impl,
	};
	nx_input_device_init(&pad->base, NX_INPUT_DEVICE_TABLET_PAD, name);

	wl_signal_init(&pad->events.button);
	wl_signal_init(&pad->events.ring);
	wl_signal_init(&pad->events.strip);

	wl_list_init(&pad->groups);
	wl_array_init(&pad->paths);
}

void nx_tablet_pad_finish(struct nx_tablet_pad *pad) {
	nx_input_device_finish(&pad->base);

	assert(wl_list_empty(&pad->events.button.listener_list));
	assert(wl_list_empty(&pad->events.ring.listener_list));
	assert(wl_list_empty(&pad->events.strip.listener_list));

	char **path_ptr;
	wl_array_for_each(path_ptr, &pad->paths) {
		free(*path_ptr);
	}
	wl_array_release(&pad->paths);

	if (!wl_list_empty(&pad->groups)) {
		nx_log(NX_ERROR, "nx_tablet_pad groups is not empty");
	}
}
