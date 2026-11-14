#include <assert.h>
#include <wayland-server-core.h>
#include "platform/clipboard/data_device/drag.h"

static void drag_handle_pointer_enter(struct nx_seat_pointer_grab *grab,
		struct nx_surface *surface, double sx, double sy) {
	drag_set_focus(grab->data, surface, sx, sy);
}

static void drag_handle_pointer_clear_focus(struct nx_seat_pointer_grab *grab) {
	drag_set_focus(grab->data, NULL, 0, 0);
}

static void drag_handle_pointer_motion(struct nx_seat_pointer_grab *grab,
		uint32_t time, double sx, double sy) {
	struct nx_drag *drag = grab->data;
	if (drag->focus == NULL || drag->focus_client == NULL) {
		return;
	}

	struct wl_resource *resource;
	wl_resource_for_each(resource, &drag->focus_client->data_devices) {
		wl_data_device_send_motion(resource, time, wl_fixed_from_double(sx),
			wl_fixed_from_double(sy));
	}

	struct nx_drag_motion_event event = {
		.drag = drag,
		.time = time,
		.sx = sx,
		.sy = sy,
	};
	wl_signal_emit_mutable(&drag->events.motion, &event);
}

static void drag_drop(struct nx_drag *drag, uint32_t time) {
	assert(drag->focus_client);

	drag->dropped = true;

	struct wl_resource *resource;
	wl_resource_for_each(resource, &drag->focus_client->data_devices) {
		wl_data_device_send_drop(resource);
	}
	if (drag->source) {
		nx_data_source_dnd_drop(drag->source);
	}

	struct nx_drag_drop_event event = {
		.drag = drag,
		.time = time,
	};
	wl_signal_emit_mutable(&drag->events.drop, &event);
}

// True if drag was freed.
static bool drag_release_source(struct nx_drag *drag, uint32_t time) {
	if (drag->focus_client && drag->source->current_dnd_action &&
			drag->source->accepted) {
		drag_drop(drag, time);
		return false;
	}
	if (!drag->source->impl->dnd_finish) {
		return false;
	}
	nx_data_source_destroy(drag->source);
	return true;
}

static uint32_t drag_handle_pointer_button(struct nx_seat_pointer_grab *grab,
		uint32_t time, uint32_t button, uint32_t state) {
	struct nx_drag *drag = grab->data;
	if (state != WL_POINTER_BUTTON_STATE_RELEASED) {
		return 0;
	}

	if (drag->source && grab->seat->pointer_state.grab_button == button &&
			drag_release_source(drag, time)) {
		return 0;
	}

	if (grab->seat->pointer_state.button_count == 0) {
		drag_destroy(drag);
	}
	return 0;
}

static void drag_handle_pointer_axis(struct nx_seat_pointer_grab *grab,
		uint32_t time, enum wl_pointer_axis orientation, double value,
		int32_t value_discrete, enum wl_pointer_axis_source source,
		enum wl_pointer_axis_relative_direction relative_direction) {
}

static void drag_handle_pointer_cancel(struct nx_seat_pointer_grab *grab) {
	drag_destroy(grab->data);
}

const struct nx_pointer_grab_interface drag_pointer_grab_impl = {
	.enter = drag_handle_pointer_enter,
	.clear_focus = drag_handle_pointer_clear_focus,
	.motion = drag_handle_pointer_motion,
	.button = drag_handle_pointer_button,
	.axis = drag_handle_pointer_axis,
	.cancel = drag_handle_pointer_cancel,
};

// Drags swallow keyboard input.
static void drag_handle_keyboard_enter(struct nx_seat_keyboard_grab *grab,
		struct nx_surface *surface, const uint32_t keycodes[],
		size_t num_keycodes, const struct nx_keyboard_modifiers *modifiers) {
}

static void drag_handle_keyboard_clear_focus(struct nx_seat_keyboard_grab *grab) {
}

static void drag_handle_keyboard_key(struct nx_seat_keyboard_grab *grab,
		uint32_t time, uint32_t key, uint32_t state) {
}

static void drag_handle_keyboard_modifiers(struct nx_seat_keyboard_grab *grab,
		const struct nx_keyboard_modifiers *modifiers) {
}

static void drag_handle_keyboard_cancel(struct nx_seat_keyboard_grab *grab) {
	drag_destroy(grab->data);
}

const struct nx_keyboard_grab_interface drag_keyboard_grab_impl = {
	.enter = drag_handle_keyboard_enter,
	.clear_focus = drag_handle_keyboard_clear_focus,
	.key = drag_handle_keyboard_key,
	.modifiers = drag_handle_keyboard_modifiers,
	.cancel = drag_handle_keyboard_cancel,
};
