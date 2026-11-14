#include <assert.h>
#include <libinput.h>
#include <stdlib.h>
#include <string.h>
#include "platform/input/libinput/libinput_device.h"
#include "platform/input/tablet/pad_interface.h"
#include "platform/log/log.h"

typedef int (*mode_group_has)(struct libinput_tablet_pad_mode_group *group,
	unsigned int index);

const struct nx_tablet_pad_impl libinput_tablet_pad_impl = {
	.name = "libinput-tablet-pad",
};

static void group_destroy(struct nx_tablet_pad_group *group) {
	free(group->buttons);
	free(group->strips);
	free(group->rings);
	free(group);
}

struct index_list {
	unsigned int *items;
	size_t count;
};

// Group's indices among total features.
static struct index_list group_indices(struct libinput_tablet_pad_mode_group *li_group,
		size_t total, mode_group_has has) {
	struct index_list list = {0};
	for (size_t i = 0; i < total; ++i) {
		if (has(li_group, i)) {
			++list.count;
		}
	}
	list.items = calloc(list.count, sizeof(unsigned int));
	if (list.items == NULL) {
		return list;
	}

	size_t n = 0;
	for (size_t i = 0; i < total; ++i) {
		if (has(li_group, i)) {
			list.items[n++] = i;
		}
	}
	return list;
}

static bool fill_group(struct nx_tablet_pad_group *group, struct nx_tablet_pad *pad,
		struct libinput_tablet_pad_mode_group *li_group) {
	struct index_list rings = group_indices(li_group, pad->ring_count,
		libinput_tablet_pad_mode_group_has_ring);
	group->rings = rings.items;
	group->ring_count = rings.count;
	if (group->rings == NULL) {
		return false;
	}

	struct index_list strips = group_indices(li_group, pad->strip_count,
		libinput_tablet_pad_mode_group_has_strip);
	group->strips = strips.items;
	group->strip_count = strips.count;
	if (group->strips == NULL) {
		return false;
	}

	struct index_list buttons = group_indices(li_group, pad->button_count,
		libinput_tablet_pad_mode_group_has_button);
	group->buttons = buttons.items;
	group->button_count = buttons.count;
	return group->buttons != NULL;
}

static void add_pad_group_from_libinput(struct nx_tablet_pad *pad,
		struct libinput_device *device, unsigned int index) {
	struct libinput_tablet_pad_mode_group *li_group =
		libinput_device_tablet_pad_get_mode_group(device, index);
	struct nx_tablet_pad_group *group = calloc(1, sizeof(*group));
	if (!group) {
		nx_log_errno(NX_ERROR, "failed to allocate nx_tablet_pad_group");
		return;
	}

	if (!fill_group(group, pad, li_group)) {
		nx_log(NX_ERROR, "failed to configure nx_tablet_pad_group");
		group_destroy(group);
		return;
	}

	group->mode_count = libinput_tablet_pad_mode_group_get_num_modes(li_group);

	libinput_tablet_pad_mode_group_ref(li_group);

	wl_list_insert(&pad->groups, &group->link);
}

struct nx_input_device *init_device_tablet_pad(
		struct nx_libinput_input_device *dev) {
	struct libinput_device *handle = dev->handle;
	const char *name = get_libinput_device_name(handle);
	struct nx_tablet_pad *nx_tablet_pad = &dev->tablet_pad;
	nx_tablet_pad_init(nx_tablet_pad, &libinput_tablet_pad_impl, name);

	nx_tablet_pad->button_count =
		libinput_device_tablet_pad_get_num_buttons(handle);
	nx_tablet_pad->ring_count =
		libinput_device_tablet_pad_get_num_rings(handle);
	nx_tablet_pad->strip_count =
		libinput_device_tablet_pad_get_num_strips(handle);

	struct udev_device *udev = libinput_device_get_udev_device(handle);
	char **dst = wl_array_add(&nx_tablet_pad->paths, sizeof(char *));
	*dst = strdup(udev_device_get_syspath(udev));
	udev_device_unref(udev);

	int groups = libinput_device_tablet_pad_get_num_mode_groups(handle);
	for (int i = 0; i < groups; ++i) {
		add_pad_group_from_libinput(nx_tablet_pad, handle, i);
	}
	return &nx_tablet_pad->base;
}

void finish_device_tablet_pad(struct nx_libinput_input_device *dev) {
	struct nx_tablet_pad_group *group, *next;
	wl_list_for_each_safe(group, next, &dev->tablet_pad.groups, link) {
		group_destroy(group);
	}

	nx_tablet_pad_finish(&dev->tablet_pad);

	int groups = libinput_device_tablet_pad_get_num_mode_groups(dev->handle);
	for (int i = 0; i < groups; ++i) {
		struct libinput_tablet_pad_mode_group *li_group =
			libinput_device_tablet_pad_get_mode_group(dev->handle, i);
		libinput_tablet_pad_mode_group_unref(li_group);
	}
}

struct nx_libinput_input_device *device_from_tablet_pad(
		struct nx_tablet_pad *nx_tablet_pad) {
	assert(nx_tablet_pad->impl == &libinput_tablet_pad_impl);

	struct nx_libinput_input_device *dev =
		wl_container_of(nx_tablet_pad, dev, tablet_pad);
	return dev;
}

void handle_tablet_pad_button(struct libinput_event *event,
		struct nx_tablet_pad *tablet_pad) {
	struct libinput_event_tablet_pad *pevent =
		libinput_event_get_tablet_pad_event(event);
	struct nx_tablet_pad_button_event nx_event = {
		.time_msec = usec_to_msec(libinput_event_tablet_pad_get_time_usec(pevent)),
		.button = libinput_event_tablet_pad_get_button_number(pevent),
		.mode = libinput_event_tablet_pad_get_mode(pevent),
		.group = libinput_tablet_pad_mode_group_get_index(
			libinput_event_tablet_pad_get_mode_group(pevent)),
	};
	if (!button_state_from_libinput(libinput_event_tablet_pad_get_button_state(pevent),
			&nx_event.state)) {
		nx_log(NX_DEBUG, "Unhandled libinput button state");
		return;
	}
	wl_signal_emit_mutable(&tablet_pad->events.button, &nx_event);
}

void handle_tablet_pad_ring(struct libinput_event *event,
		struct nx_tablet_pad *tablet_pad) {
	struct libinput_event_tablet_pad *pevent =
		libinput_event_get_tablet_pad_event(event);
	struct nx_tablet_pad_ring_event nx_event = {
		.time_msec = usec_to_msec(libinput_event_tablet_pad_get_time_usec(pevent)),
		.ring = libinput_event_tablet_pad_get_ring_number(pevent),
		.position = libinput_event_tablet_pad_get_ring_position(pevent),
		.mode = libinput_event_tablet_pad_get_mode(pevent),
		.source = NX_TABLET_PAD_RING_SOURCE_UNKNOWN,
	};
	if (libinput_event_tablet_pad_get_ring_source(pevent) ==
			LIBINPUT_TABLET_PAD_RING_SOURCE_FINGER) {
		nx_event.source = NX_TABLET_PAD_RING_SOURCE_FINGER;
	}
	wl_signal_emit_mutable(&tablet_pad->events.ring, &nx_event);
}

void handle_tablet_pad_strip(struct libinput_event *event,
		struct nx_tablet_pad *tablet_pad) {
	struct libinput_event_tablet_pad *pevent =
		libinput_event_get_tablet_pad_event(event);
	struct nx_tablet_pad_strip_event nx_event = {
		.time_msec = usec_to_msec(libinput_event_tablet_pad_get_time_usec(pevent)),
		.strip = libinput_event_tablet_pad_get_strip_number(pevent),
		.position = libinput_event_tablet_pad_get_strip_position(pevent),
		.mode = libinput_event_tablet_pad_get_mode(pevent),
		.source = NX_TABLET_PAD_STRIP_SOURCE_UNKNOWN,
	};
	if (libinput_event_tablet_pad_get_strip_source(pevent) ==
			LIBINPUT_TABLET_PAD_STRIP_SOURCE_FINGER) {
		nx_event.source = NX_TABLET_PAD_STRIP_SOURCE_FINGER;
	}
	wl_signal_emit_mutable(&tablet_pad->events.strip, &nx_event);
}
