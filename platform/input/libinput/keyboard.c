#include <assert.h>
#include <libinput.h>
#include "platform/input/keyboard/keyboard_interface.h"
#include "platform/input/libinput/libinput_device.h"
#include "platform/log/log.h"

struct nx_libinput_input_device *device_from_keyboard(
		struct nx_keyboard *kb) {
	assert(kb->impl == &libinput_keyboard_impl);

	struct nx_libinput_input_device *dev = wl_container_of(kb, dev, keyboard);
	return dev;
}

static void keyboard_set_leds(struct nx_keyboard *nx_kb, uint32_t leds) {
	struct nx_libinput_input_device *dev = device_from_keyboard(nx_kb);
	libinput_device_led_update(dev->handle, leds);
}

const struct nx_keyboard_impl libinput_keyboard_impl = {
	.name = "libinput-keyboard",
	.led_update = keyboard_set_leds
};

struct nx_input_device *init_device_keyboard(
		struct nx_libinput_input_device *dev) {
	const char *name = get_libinput_device_name(dev->handle);
	nx_keyboard_init(&dev->keyboard, &libinput_keyboard_impl, name);

	libinput_device_led_update(dev->handle, 0);
	return &dev->keyboard.base;
}

static bool key_state_from_libinput(enum libinput_key_state state,
		enum wl_keyboard_key_state *out) {
	switch (state) {
	case LIBINPUT_KEY_STATE_RELEASED:
		*out = WL_KEYBOARD_KEY_STATE_RELEASED;
		return true;
	case LIBINPUT_KEY_STATE_PRESSED:
		*out = WL_KEYBOARD_KEY_STATE_PRESSED;
		return true;
	}
	return false;
}

void handle_keyboard_key(struct libinput_event *event,
		struct nx_keyboard *kb) {
	struct libinput_event_keyboard *kbevent =
		libinput_event_get_keyboard_event(event);
	struct nx_keyboard_key_event nx_event = {
		.time_msec = usec_to_msec(libinput_event_keyboard_get_time_usec(kbevent)),
		.keycode = libinput_event_keyboard_get_key(kbevent),
		.update_state = true,
	};
	if (!key_state_from_libinput(libinput_event_keyboard_get_key_state(kbevent),
			&nx_event.state)) {
		nx_log(NX_DEBUG, "Unhandled libinput key state");
		return;
	}
	nx_keyboard_notify_key(kb, &nx_event);
}
