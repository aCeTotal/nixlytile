#include <libinput.h>
#include <stdlib.h>
#include "platform/input/libinput/libinput_device.h"
#include "platform/log/log.h"

static const struct {
	enum libinput_device_capability capability;
	struct nx_input_device *(*init)(struct nx_libinput_input_device *dev);
} device_types[] = {
	{ LIBINPUT_DEVICE_CAP_KEYBOARD, init_device_keyboard },
	{ LIBINPUT_DEVICE_CAP_POINTER, init_device_pointer },
	{ LIBINPUT_DEVICE_CAP_SWITCH, init_device_switch },
	{ LIBINPUT_DEVICE_CAP_TABLET_TOOL, init_device_tablet },
	{ LIBINPUT_DEVICE_CAP_TABLET_PAD, init_device_tablet_pad },
};

static void handle_device_added(struct nx_libinput_backend *backend,
		struct libinput_device *libinput_dev) {
	int vendor = libinput_device_get_id_vendor(libinput_dev);
	int product = libinput_device_get_id_product(libinput_dev);
	const char *name = libinput_device_get_name(libinput_dev);
	nx_log(NX_DEBUG, "Adding %s [%d:%d]", name, vendor, product);

	struct nx_libinput_input_device *dev = calloc(1, sizeof(*dev));
	if (dev == NULL) {
		nx_log_errno(NX_ERROR, "failed to allocate nx_libinput_input_device");
		return;
	}

	dev->handle = libinput_dev;
	libinput_device_ref(libinput_dev);
	libinput_device_set_user_data(libinput_dev, dev);

	wl_list_insert(&backend->devices, &dev->link);

	for (size_t i = 0; i < sizeof(device_types) / sizeof(device_types[0]); i++) {
		if (!libinput_device_has_capability(libinput_dev,
				device_types[i].capability)) {
			continue;
		}
		struct nx_input_device *input = device_types[i].init(dev);
		wl_signal_emit_mutable(&backend->backend.events.new_input, input);
	}
}

static void handle_device_removed(struct nx_libinput_input_device *dev) {
	int vendor = libinput_device_get_id_vendor(dev->handle);
	int product = libinput_device_get_id_product(dev->handle);
	const char *name = libinput_device_get_name(dev->handle);
	nx_log(NX_DEBUG, "Removing %s [%d:%d]", name, vendor, product);

	destroy_libinput_input_device(dev);
}

void handle_libinput_event(struct nx_libinput_backend *backend,
		struct libinput_event *event) {
	struct libinput_device *libinput_dev = libinput_event_get_device(event);
	struct nx_libinput_input_device *dev =
		libinput_device_get_user_data(libinput_dev);
	enum libinput_event_type event_type = libinput_event_get_type(event);

	if (dev == NULL && event_type != LIBINPUT_EVENT_DEVICE_ADDED) {
		nx_log(NX_ERROR, "libinput_device has no nx_libinput_input_device");
		return;
	}

	switch (event_type) {
	case LIBINPUT_EVENT_DEVICE_ADDED:
		handle_device_added(backend, libinput_dev);
		break;
	case LIBINPUT_EVENT_DEVICE_REMOVED:
		handle_device_removed(dev);
		break;
	case LIBINPUT_EVENT_KEYBOARD_KEY:
		handle_keyboard_key(event, &dev->keyboard);
		break;
	case LIBINPUT_EVENT_POINTER_MOTION:
		handle_pointer_motion(event, &dev->pointer);
		break;
	case LIBINPUT_EVENT_POINTER_MOTION_ABSOLUTE:
		handle_pointer_motion_abs(event, &dev->pointer);
		break;
	case LIBINPUT_EVENT_POINTER_BUTTON:
		handle_pointer_button(event, &dev->pointer);
		break;
	case LIBINPUT_EVENT_POINTER_AXIS:
		// Superseded by the SCROLL_* events.
		break;
	case LIBINPUT_EVENT_POINTER_SCROLL_WHEEL:
		handle_pointer_axis_value120(event, &dev->pointer,
			WL_POINTER_AXIS_SOURCE_WHEEL);
		break;
	case LIBINPUT_EVENT_POINTER_SCROLL_FINGER:
		handle_pointer_axis_value120(event, &dev->pointer,
			WL_POINTER_AXIS_SOURCE_FINGER);
		break;
	case LIBINPUT_EVENT_POINTER_SCROLL_CONTINUOUS:
		handle_pointer_axis_value120(event, &dev->pointer,
			WL_POINTER_AXIS_SOURCE_CONTINUOUS);
		break;
	case LIBINPUT_EVENT_TOUCH_DOWN:
	case LIBINPUT_EVENT_TOUCH_UP:
	case LIBINPUT_EVENT_TOUCH_MOTION:
	case LIBINPUT_EVENT_TOUCH_CANCEL:
	case LIBINPUT_EVENT_TOUCH_FRAME:
		// Touchscreens are ignored.
		break;
	case LIBINPUT_EVENT_TABLET_TOOL_AXIS:
		handle_tablet_tool_axis(event, &dev->tablet);
		break;
	case LIBINPUT_EVENT_TABLET_TOOL_PROXIMITY:
		handle_tablet_tool_proximity(event, &dev->tablet);
		break;
	case LIBINPUT_EVENT_TABLET_TOOL_TIP:
		handle_tablet_tool_tip(event, &dev->tablet);
		break;
	case LIBINPUT_EVENT_TABLET_TOOL_BUTTON:
		handle_tablet_tool_button(event, &dev->tablet);
		break;
	case LIBINPUT_EVENT_TABLET_PAD_BUTTON:
		handle_tablet_pad_button(event, &dev->tablet_pad);
		break;
	case LIBINPUT_EVENT_TABLET_PAD_RING:
		handle_tablet_pad_ring(event, &dev->tablet_pad);
		break;
	case LIBINPUT_EVENT_TABLET_PAD_STRIP:
		handle_tablet_pad_strip(event, &dev->tablet_pad);
		break;
	case LIBINPUT_EVENT_SWITCH_TOGGLE:
		handle_switch_toggle(event, &dev->switch_device);
		break;
	case LIBINPUT_EVENT_GESTURE_SWIPE_BEGIN:
		handle_pointer_swipe_begin(event, &dev->pointer);
		break;
	case LIBINPUT_EVENT_GESTURE_SWIPE_UPDATE:
		handle_pointer_swipe_update(event, &dev->pointer);
		break;
	case LIBINPUT_EVENT_GESTURE_SWIPE_END:
		handle_pointer_swipe_end(event, &dev->pointer);
		break;
	case LIBINPUT_EVENT_GESTURE_PINCH_BEGIN:
		handle_pointer_pinch_begin(event, &dev->pointer);
		break;
	case LIBINPUT_EVENT_GESTURE_PINCH_UPDATE:
		handle_pointer_pinch_update(event, &dev->pointer);
		break;
	case LIBINPUT_EVENT_GESTURE_PINCH_END:
		handle_pointer_pinch_end(event, &dev->pointer);
		break;
	case LIBINPUT_EVENT_GESTURE_HOLD_BEGIN:
		handle_pointer_hold_begin(event, &dev->pointer);
		break;
	case LIBINPUT_EVENT_GESTURE_HOLD_END:
		handle_pointer_hold_end(event, &dev->pointer);
		break;
	default:
		nx_log(NX_DEBUG, "Unknown libinput event %d", event_type);
		break;
	}
}

bool button_state_from_libinput(enum libinput_button_state state,
		enum nx_button_state *out) {
	switch (state) {
	case LIBINPUT_BUTTON_STATE_RELEASED:
		*out = NX_BUTTON_RELEASED;
		return true;
	case LIBINPUT_BUTTON_STATE_PRESSED:
		*out = NX_BUTTON_PRESSED;
		return true;
	}
	return false;
}
