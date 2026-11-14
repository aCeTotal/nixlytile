#include <stdlib.h>
#include "platform/shell/popup/popup_grab.h"
#include "xdg-shell-protocol.h"

static void popup_grab_end(struct nx_xdg_popup_grab *popup_grab) {
	struct nx_xdg_popup *popup, *tmp;
	wl_list_for_each_safe(popup, tmp, &popup_grab->popups, grab_link) {
		xdg_popup_send_popup_done(popup->resource);
	}

	nx_seat_pointer_end_grab(popup_grab->seat);
	nx_seat_keyboard_end_grab(popup_grab->seat);
}

static void pointer_grab_enter(struct nx_seat_pointer_grab *grab,
		struct nx_surface *surface, double sx, double sy) {
	struct nx_xdg_popup_grab *popup_grab = grab->data;
	if (wl_resource_get_client(surface->resource) != popup_grab->client) {
		nx_seat_pointer_clear_focus(grab->seat);
		return;
	}
	nx_seat_pointer_enter(grab->seat, surface, sx, sy);
}

static void pointer_grab_clear_focus(struct nx_seat_pointer_grab *grab) {
	nx_seat_pointer_clear_focus(grab->seat);
}

static void pointer_grab_motion(struct nx_seat_pointer_grab *grab,
		uint32_t time, double sx, double sy) {
	nx_seat_pointer_send_motion(grab->seat, time, sx, sy);
}

static uint32_t pointer_grab_button(struct nx_seat_pointer_grab *grab,
		uint32_t time, uint32_t button, uint32_t state) {
	uint32_t serial =
		nx_seat_pointer_send_button(grab->seat, time, button, state);
	// Click outside the client dismisses.
	if (serial == 0) {
		popup_grab_end(grab->data);
	}
	return serial;
}

static void pointer_grab_axis(struct nx_seat_pointer_grab *grab,
		uint32_t time, enum wl_pointer_axis orientation, double value,
		int32_t value_discrete, enum wl_pointer_axis_source source,
		enum wl_pointer_axis_relative_direction relative_direction) {
	nx_seat_pointer_send_axis(grab->seat, time, orientation, value,
		value_discrete, source, relative_direction);
}

static void pointer_grab_frame(struct nx_seat_pointer_grab *grab) {
	nx_seat_pointer_send_frame(grab->seat);
}

static void pointer_grab_cancel(struct nx_seat_pointer_grab *grab) {
	popup_grab_end(grab->data);
}

static const struct nx_pointer_grab_interface pointer_grab_impl = {
	.enter = pointer_grab_enter,
	.clear_focus = pointer_grab_clear_focus,
	.motion = pointer_grab_motion,
	.button = pointer_grab_button,
	.cancel = pointer_grab_cancel,
	.axis = pointer_grab_axis,
	.frame = pointer_grab_frame,
};

// Focus stays on the popup.
static void keyboard_grab_enter(struct nx_seat_keyboard_grab *grab,
		struct nx_surface *surface, const uint32_t keycodes[], size_t num_keycodes,
		const struct nx_keyboard_modifiers *modifiers) {
}

static void keyboard_grab_clear_focus(struct nx_seat_keyboard_grab *grab) {
}

static void keyboard_grab_key(struct nx_seat_keyboard_grab *grab, uint32_t time,
		uint32_t key, uint32_t state) {
	nx_seat_keyboard_send_key(grab->seat, time, key, state);
}

static void keyboard_grab_modifiers(struct nx_seat_keyboard_grab *grab,
		const struct nx_keyboard_modifiers *modifiers) {
	nx_seat_keyboard_send_modifiers(grab->seat, modifiers);
}

// Ends both grabs via pointer.
static void keyboard_grab_cancel(struct nx_seat_keyboard_grab *grab) {
	nx_seat_pointer_end_grab(grab->seat);
}

static const struct nx_keyboard_grab_interface keyboard_grab_impl = {
	.enter = keyboard_grab_enter,
	.clear_focus = keyboard_grab_clear_focus,
	.key = keyboard_grab_key,
	.modifiers = keyboard_grab_modifiers,
	.cancel = keyboard_grab_cancel,
};

static void popup_grab_destroy(struct nx_xdg_popup_grab *popup_grab) {
	wl_list_remove(&popup_grab->seat_destroy.link);

	struct nx_xdg_popup *popup, *tmp;
	wl_list_for_each_safe(popup, tmp, &popup_grab->popups, grab_link) {
		nx_xdg_popup_destroy(popup);
	}

	wl_list_remove(&popup_grab->link);
	free(popup_grab);
}

static void handle_seat_destroy(struct wl_listener *listener, void *data) {
	struct nx_xdg_popup_grab *popup_grab =
		wl_container_of(listener, popup_grab, seat_destroy);
	popup_grab_destroy(popup_grab);
}

struct nx_xdg_popup_grab *shell_popup_grab_from_seat(
		struct nx_xdg_shell *shell, struct nx_seat *seat) {
	struct nx_xdg_popup_grab *popup_grab;
	wl_list_for_each(popup_grab, &shell->popup_grabs, link) {
		if (popup_grab->seat == seat) {
			return popup_grab;
		}
	}

	popup_grab = calloc(1, sizeof(*popup_grab));
	if (!popup_grab) {
		return NULL;
	}

	popup_grab->pointer_grab.data = popup_grab;
	popup_grab->pointer_grab.interface = &pointer_grab_impl;
	popup_grab->keyboard_grab.data = popup_grab;
	popup_grab->keyboard_grab.interface = &keyboard_grab_impl;

	wl_list_init(&popup_grab->popups);
	wl_list_insert(&shell->popup_grabs, &popup_grab->link);
	popup_grab->seat = seat;

	popup_grab->seat_destroy.notify = handle_seat_destroy;
	wl_signal_add(&seat->events.destroy, &popup_grab->seat_destroy);

	return popup_grab;
}

void shell_popup_grab_add(struct nx_xdg_popup_grab *grab,
		struct nx_xdg_popup *popup) {
	grab->client = popup->base->client->client;
	popup->seat = grab->seat;

	wl_list_insert(&grab->popups, &popup->grab_link);

	nx_seat_pointer_start_grab(grab->seat, &grab->pointer_grab);
	nx_seat_keyboard_start_grab(grab->seat, &grab->keyboard_grab);
}

void shell_popup_grab_remove(struct nx_xdg_popup *popup) {
	struct nx_xdg_popup_grab *grab = shell_popup_grab_from_seat(
		popup->base->client->shell, popup->seat);
	wl_list_remove(&popup->grab_link);
	if (!wl_list_empty(&grab->popups)) {
		return;
	}

	if (grab->seat->pointer_state.grab == &grab->pointer_grab) {
		nx_seat_pointer_end_grab(grab->seat);
	}
	if (grab->seat->keyboard_state.grab == &grab->keyboard_grab) {
		nx_seat_keyboard_end_grab(grab->seat);
	}
	popup_grab_destroy(grab);
}
