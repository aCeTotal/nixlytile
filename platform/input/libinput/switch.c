#include <assert.h>
#include <libinput.h>
#include "platform/input/device/switch_interface.h"
#include "platform/input/libinput/libinput_device.h"
#include "platform/log/log.h"

const struct nx_switch_impl libinput_switch_impl = {
	.name = "libinput-switch",
};

struct nx_input_device *init_device_switch(
		struct nx_libinput_input_device *dev) {
	const char *name = get_libinput_device_name(dev->handle);
	nx_switch_init(&dev->switch_device, &libinput_switch_impl, name);
	return &dev->switch_device.base;
}

struct nx_libinput_input_device *device_from_switch(
		struct nx_switch *nx_switch) {
	assert(nx_switch->impl == &libinput_switch_impl);

	struct nx_libinput_input_device *dev =
		wl_container_of(nx_switch, dev, switch_device);
	return dev;
}

static bool switch_type_from_libinput(enum libinput_switch type,
		enum nx_switch_type *out) {
	switch (type) {
	case LIBINPUT_SWITCH_LID:
		*out = NX_SWITCH_TYPE_LID;
		return true;
	case LIBINPUT_SWITCH_TABLET_MODE:
		*out = NX_SWITCH_TYPE_TABLET_MODE;
		return true;
	default:
		return false;
	}
}

static bool switch_state_from_libinput(enum libinput_switch_state state,
		enum nx_switch_state *out) {
	switch (state) {
	case LIBINPUT_SWITCH_STATE_OFF:
		*out = NX_SWITCH_STATE_OFF;
		return true;
	case LIBINPUT_SWITCH_STATE_ON:
		*out = NX_SWITCH_STATE_ON;
		return true;
	}
	return false;
}

void handle_switch_toggle(struct libinput_event *event,
		struct nx_switch *nx_switch) {
	struct libinput_event_switch *sevent =
		libinput_event_get_switch_event(event);
	struct nx_switch_toggle_event nx_event = {
		.time_msec = usec_to_msec(libinput_event_switch_get_time_usec(sevent)),
	};
	if (!switch_type_from_libinput(libinput_event_switch_get_switch(sevent),
			&nx_event.switch_type)) {
		nx_log(NX_DEBUG, "Unhandled libinput switch type");
		return;
	}
	if (!switch_state_from_libinput(libinput_event_switch_get_switch_state(sevent),
			&nx_event.switch_state)) {
		nx_log(NX_DEBUG, "Unhandled libinput switch state");
		return;
	}
	wl_signal_emit_mutable(&nx_switch->events.toggle, &nx_event);
}
