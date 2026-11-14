#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-server-core.h>
#include "platform/clipboard/data_device/data_device.h"
#include "platform/clipboard/primary/primary_selection.h"
#include "platform/input/seat/client.h"
#include "platform/protocol/global.h"

#define SEAT_VERSION 9

static void default_pointer_enter(struct nx_seat_pointer_grab *grab,
		struct nx_surface *surface, double sx, double sy) {
	nx_seat_pointer_enter(grab->seat, surface, sx, sy);
}

static void default_pointer_clear_focus(struct nx_seat_pointer_grab *grab) {
	nx_seat_pointer_clear_focus(grab->seat);
}

static void default_pointer_motion(struct nx_seat_pointer_grab *grab,
		uint32_t time, double sx, double sy) {
	nx_seat_pointer_send_motion(grab->seat, time, sx, sy);
}

static uint32_t default_pointer_button(struct nx_seat_pointer_grab *grab,
		uint32_t time, uint32_t button, enum wl_pointer_button_state state) {
	return nx_seat_pointer_send_button(grab->seat, time, button, state);
}

static void default_pointer_axis(struct nx_seat_pointer_grab *grab,
		uint32_t time, enum wl_pointer_axis orientation, double value,
		int32_t value_discrete, enum wl_pointer_axis_source source,
		enum wl_pointer_axis_relative_direction relative_direction) {
	nx_seat_pointer_send_axis(grab->seat, time, orientation, value,
		value_discrete, source, relative_direction);
}

static void default_pointer_frame(struct nx_seat_pointer_grab *grab) {
	nx_seat_pointer_send_frame(grab->seat);
}

static void default_pointer_cancel(struct nx_seat_pointer_grab *grab) {
	// The default grab never ends.
}

static const struct nx_pointer_grab_interface default_pointer_grab_impl = {
	.enter = default_pointer_enter,
	.clear_focus = default_pointer_clear_focus,
	.motion = default_pointer_motion,
	.button = default_pointer_button,
	.axis = default_pointer_axis,
	.frame = default_pointer_frame,
	.cancel = default_pointer_cancel,
};

static void default_keyboard_enter(struct nx_seat_keyboard_grab *grab,
		struct nx_surface *surface, const uint32_t keycodes[], size_t num_keycodes,
		const struct nx_keyboard_modifiers *modifiers) {
	nx_seat_keyboard_enter(grab->seat, surface, keycodes, num_keycodes, modifiers);
}

static void default_keyboard_clear_focus(struct nx_seat_keyboard_grab *grab) {
	nx_seat_keyboard_clear_focus(grab->seat);
}

static void default_keyboard_key(struct nx_seat_keyboard_grab *grab,
		uint32_t time, uint32_t key, uint32_t state) {
	nx_seat_keyboard_send_key(grab->seat, time, key, state);
}

static void default_keyboard_modifiers(struct nx_seat_keyboard_grab *grab,
		const struct nx_keyboard_modifiers *modifiers) {
	nx_seat_keyboard_send_modifiers(grab->seat, modifiers);
}

static void default_keyboard_cancel(struct nx_seat_keyboard_grab *grab) {
	// The default grab never ends.
}

static const struct nx_keyboard_grab_interface default_keyboard_grab_impl = {
	.enter = default_keyboard_enter,
	.clear_focus = default_keyboard_clear_focus,
	.key = default_keyboard_key,
	.modifiers = default_keyboard_modifiers,
	.cancel = default_keyboard_cancel,
};

static void seat_destroy(struct nx_seat *seat) {
	nx_seat_pointer_clear_focus(seat);
	nx_seat_keyboard_clear_focus(seat);
	nx_seat_set_keyboard(seat, NULL);

	wl_signal_emit_mutable(&seat->events.destroy, seat);

	assert(wl_list_empty(&seat->keyboard_state.events.focus_change.listener_list));
	assert(wl_list_empty(&seat->events.request_start_drag.listener_list));
	assert(wl_list_empty(&seat->events.start_drag.listener_list));
	assert(wl_list_empty(&seat->events.request_set_cursor.listener_list));
	assert(wl_list_empty(&seat->events.request_set_selection.listener_list));
	assert(wl_list_empty(&seat->events.set_selection.listener_list));
	assert(wl_list_empty(&seat->events.request_set_primary_selection.listener_list));
	assert(wl_list_empty(&seat->events.set_primary_selection.listener_list));
	assert(wl_list_empty(&seat->events.destroy.listener_list));

	wl_list_remove(&seat->display_destroy.link);

	nx_data_source_destroy(seat->selection_source);
	nx_primary_selection_source_destroy(seat->primary_selection_source);

	struct nx_seat_client *client, *tmp;
	wl_list_for_each_safe(client, tmp, &seat->clients, link) {
		seat_client_destroy(client);
	}

	nx_global_destroy_safe(seat->global);
	free(seat->pointer_state.default_grab);
	free(seat->keyboard_state.default_grab);
	free(seat->name);
	free(seat);
}

static void handle_display_destroy(struct wl_listener *listener, void *data) {
	struct nx_seat *seat = wl_container_of(listener, seat, display_destroy);
	seat_destroy(seat);
}

static void init_pointer_state(struct nx_seat *seat,
		struct nx_seat_pointer_grab *grab) {
	grab->interface = &default_pointer_grab_impl;
	grab->seat = seat;
	seat->pointer_state.seat = seat;
	seat->pointer_state.default_grab = grab;
	seat->pointer_state.grab = grab;
	wl_list_init(&seat->pointer_state.surface_destroy.link);
}

static void init_keyboard_state(struct nx_seat *seat,
		struct nx_seat_keyboard_grab *grab) {
	grab->interface = &default_keyboard_grab_impl;
	grab->seat = seat;
	seat->keyboard_state.seat = seat;
	seat->keyboard_state.default_grab = grab;
	seat->keyboard_state.grab = grab;
	wl_list_init(&seat->keyboard_state.surface_destroy.link);
	wl_signal_init(&seat->keyboard_state.events.focus_change);
}

struct nx_seat *nx_seat_create(struct wl_display *display, const char *name) {
	struct nx_seat *seat = calloc(1, sizeof(*seat));
	struct nx_seat_pointer_grab *pointer_grab = calloc(1, sizeof(*pointer_grab));
	struct nx_seat_keyboard_grab *keyboard_grab = calloc(1, sizeof(*keyboard_grab));
	if (seat == NULL || pointer_grab == NULL || keyboard_grab == NULL) {
		goto error;
	}
	init_pointer_state(seat, pointer_grab);
	init_keyboard_state(seat, keyboard_grab);

	seat->global = wl_global_create(display, &wl_seat_interface,
		SEAT_VERSION, seat, seat_bind);
	if (seat->global == NULL) {
		goto error;
	}
	seat->display = display;
	seat->name = strdup(name);
	wl_list_init(&seat->clients);
	wl_list_init(&seat->selection_offers);
	wl_list_init(&seat->drag_offers);

	wl_signal_init(&seat->events.request_set_cursor);
	wl_signal_init(&seat->events.request_set_selection);
	wl_signal_init(&seat->events.set_selection);
	wl_signal_init(&seat->events.request_set_primary_selection);
	wl_signal_init(&seat->events.set_primary_selection);
	wl_signal_init(&seat->events.request_start_drag);
	wl_signal_init(&seat->events.start_drag);
	wl_signal_init(&seat->events.destroy);

	seat->display_destroy.notify = handle_display_destroy;
	wl_display_add_destroy_listener(display, &seat->display_destroy);
	return seat;

error:
	free(keyboard_grab);
	free(pointer_grab);
	free(seat);
	return NULL;
}

// Focus stays for re-enter.
static void client_drop_pointers(struct nx_seat_client *client) {
	struct nx_seat_pointer_state *state = &client->seat->pointer_state;
	if (state->focused_client != NULL && state->focused_surface != NULL) {
		seat_client_send_pointer_leave_raw(state->focused_client,
			state->focused_surface);
	}

	struct wl_resource *resource, *tmp;
	wl_resource_for_each_safe(resource, tmp, &client->pointers) {
		seat_resource_make_inert(resource);
	}
}

static void client_drop_keyboards(struct nx_seat_client *client) {
	struct nx_seat_keyboard_state *state = &client->seat->keyboard_state;
	if (state->focused_client != NULL && state->focused_surface != NULL) {
		seat_client_send_keyboard_leave_raw(state->focused_client,
			state->focused_surface);
	}

	struct wl_resource *resource, *tmp;
	wl_resource_for_each_safe(resource, tmp, &client->keyboards) {
		seat_resource_make_inert(resource);
	}
}

void nx_seat_set_capabilities(struct nx_seat *seat, uint32_t capabilities) {
	if (capabilities == seat->capabilities) {
		return;
	}
	seat->capabilities = capabilities;
	seat->accumulated_capabilities |= capabilities;

	struct nx_seat_client *client;
	wl_list_for_each(client, &seat->clients, link) {
		if ((capabilities & WL_SEAT_CAPABILITY_POINTER) == 0) {
			client_drop_pointers(client);
		}
		if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) == 0) {
			client_drop_keyboards(client);
		}

		struct wl_resource *resource;
		wl_resource_for_each(resource, &client->resources) {
			wl_seat_send_capabilities(resource, capabilities);
		}
	}
}
