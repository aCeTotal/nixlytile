#include <assert.h>
#include <stdlib.h>
#include <wayland-server-core.h>
#include "platform/container/set.h"
#include "platform/input/device/input_device_interface.h"
#include "platform/input/pointer/pointer_interface.h"
#include "platform/os/clock.h"

struct nx_pointer *nx_pointer_from_input_device(
		struct nx_input_device *input_device) {
	assert(input_device->type == NX_INPUT_DEVICE_POINTER);
	return wl_container_of(input_device, (struct nx_pointer *)NULL, base);
}

void nx_pointer_init(struct nx_pointer *pointer,
		const struct nx_pointer_impl *impl, const char *name) {
	*pointer = (struct nx_pointer){
		.impl = impl,
	};
	nx_input_device_init(&pointer->base, NX_INPUT_DEVICE_POINTER, name);

	wl_signal_init(&pointer->events.motion);
	wl_signal_init(&pointer->events.motion_absolute);
	wl_signal_init(&pointer->events.button);
	wl_signal_init(&pointer->events.axis);
	wl_signal_init(&pointer->events.frame);
	wl_signal_init(&pointer->events.swipe_begin);
	wl_signal_init(&pointer->events.swipe_update);
	wl_signal_init(&pointer->events.swipe_end);
	wl_signal_init(&pointer->events.pinch_begin);
	wl_signal_init(&pointer->events.pinch_update);
	wl_signal_init(&pointer->events.pinch_end);
	wl_signal_init(&pointer->events.hold_begin);
	wl_signal_init(&pointer->events.hold_end);
}

void nx_pointer_finish(struct nx_pointer *pointer) {
	int64_t time_msec = get_current_time_msec();
	while (pointer->button_count > 0) {
		struct nx_pointer_button_event event = {
			.pointer = pointer,
			.time_msec = time_msec,
			.button = pointer->buttons[pointer->button_count - 1],
			.state = WL_POINTER_BUTTON_STATE_RELEASED,
		};
		nx_pointer_notify_button(pointer, &event);
	}

	nx_input_device_finish(&pointer->base);

	assert(wl_list_empty(&pointer->events.motion.listener_list));
	assert(wl_list_empty(&pointer->events.motion_absolute.listener_list));
	assert(wl_list_empty(&pointer->events.button.listener_list));
	assert(wl_list_empty(&pointer->events.axis.listener_list));
	assert(wl_list_empty(&pointer->events.frame.listener_list));
	assert(wl_list_empty(&pointer->events.swipe_begin.listener_list));
	assert(wl_list_empty(&pointer->events.swipe_update.listener_list));
	assert(wl_list_empty(&pointer->events.swipe_end.listener_list));
	assert(wl_list_empty(&pointer->events.pinch_begin.listener_list));
	assert(wl_list_empty(&pointer->events.pinch_update.listener_list));
	assert(wl_list_empty(&pointer->events.pinch_end.listener_list));
	assert(wl_list_empty(&pointer->events.hold_begin.listener_list));
	assert(wl_list_empty(&pointer->events.hold_end.listener_list));

	free(pointer->output_name);
}

void nx_pointer_notify_button(struct nx_pointer *pointer,
		struct nx_pointer_button_event *event) {
	if (event->state == WL_POINTER_BUTTON_STATE_PRESSED) {
		set_add(pointer->buttons, &pointer->button_count,
			NX_POINTER_BUTTONS_CAP, event->button);
	} else {
		set_remove(pointer->buttons, &pointer->button_count,
			NX_POINTER_BUTTONS_CAP, event->button);
	}

	wl_signal_emit_mutable(&pointer->events.button, event);
}
