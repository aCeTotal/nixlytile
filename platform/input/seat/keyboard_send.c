#include <wayland-server-core.h>
#include "platform/clipboard/data_device/data_device.h"
#include "platform/input/seat/client.h"
#include "platform/surface/compositor.h"

static void handle_surface_destroy(struct wl_listener *listener, void *data) {
	struct nx_seat_keyboard_state *state =
		wl_container_of(listener, state, surface_destroy);
	wl_list_remove(&state->surface_destroy.link);
	wl_list_init(&state->surface_destroy.link);
	nx_seat_keyboard_clear_focus(state->seat);
}

static void watch_surface(struct nx_seat_keyboard_state *state,
		struct nx_surface *surface) {
	wl_list_remove(&state->surface_destroy.link);
	wl_list_init(&state->surface_destroy.link);
	if (surface == NULL) {
		return;
	}
	wl_signal_add(&surface->events.destroy, &state->surface_destroy);
	state->surface_destroy.notify = handle_surface_destroy;
}

void seat_client_send_keyboard_leave_raw(struct nx_seat_client *seat_client,
		struct nx_surface *surface) {
	uint32_t serial = nx_seat_client_next_serial(seat_client);
	struct wl_resource *resource;
	wl_resource_for_each(resource, &seat_client->keyboards) {
		if (seat_client_from_keyboard_resource(resource) == NULL) {
			continue;
		}
		wl_keyboard_send_leave(resource, serial, surface->resource);
	}
}

static void client_send_enter(struct nx_seat_client *client,
		struct nx_surface *surface, struct wl_array *keys) {
	uint32_t serial = nx_seat_client_next_serial(client);
	struct wl_resource *resource;
	wl_resource_for_each(resource, &client->keyboards) {
		if (seat_client_from_keyboard_resource(resource) == NULL) {
			continue;
		}
		wl_keyboard_send_enter(resource, serial, surface->resource, keys);
	}
}

void nx_seat_keyboard_enter(struct nx_seat *seat,
		struct nx_surface *surface, const uint32_t keycodes[], size_t num_keycodes,
		const struct nx_keyboard_modifiers *modifiers) {
	struct nx_seat_keyboard_state *state = &seat->keyboard_state;
	if (state->focused_surface == surface) {
		return;
	}

	struct nx_seat_client *client = NULL;
	if (surface != NULL) {
		client = nx_seat_client_for_wl_client(seat,
			wl_resource_get_client(surface->resource));
	}
	struct nx_surface *old_surface = state->focused_surface;
	if (state->focused_client != NULL && old_surface != NULL) {
		seat_client_send_keyboard_leave_raw(state->focused_client, old_surface);
	}
	if (client != NULL) {
		struct wl_array keys = {
			.data = (void *)keycodes,
			.size = num_keycodes * sizeof(keycodes[0]),
		};
		client_send_enter(client, surface, &keys);
	}

	watch_surface(state, surface);
	state->focused_client = client;
	state->focused_surface = surface;

	// Targets the new focused client.
	if (client != NULL) {
		nx_seat_keyboard_send_modifiers(seat, modifiers);
		seat_client_send_selection(client);
	}

	struct nx_seat_keyboard_focus_change_event event = {
		.seat = seat,
		.old_surface = old_surface,
		.new_surface = surface,
	};
	wl_signal_emit_mutable(&state->events.focus_change, &event);
}

void nx_seat_keyboard_clear_focus(struct nx_seat *seat) {
	nx_seat_keyboard_enter(seat, NULL, NULL, 0, NULL);
}

void nx_seat_keyboard_send_key(struct nx_seat *seat, uint32_t time,
		uint32_t key, uint32_t state) {
	struct nx_seat_client *client = seat->keyboard_state.focused_client;
	if (client == NULL) {
		return;
	}

	uint32_t serial = nx_seat_client_next_serial(client);
	struct wl_resource *resource;
	wl_resource_for_each(resource, &client->keyboards) {
		if (seat_client_from_keyboard_resource(resource) == NULL) {
			continue;
		}
		wl_keyboard_send_key(resource, serial, time, key, state);
	}
}

void nx_seat_keyboard_send_modifiers(struct nx_seat *seat,
		const struct nx_keyboard_modifiers *modifiers) {
	struct nx_seat_client *client = seat->keyboard_state.focused_client;
	if (client == NULL) {
		return;
	}

	const struct nx_keyboard_modifiers none = {0};
	if (modifiers == NULL) {
		modifiers = &none;
	}
	uint32_t serial = nx_seat_client_next_serial(client);
	struct wl_resource *resource;
	wl_resource_for_each(resource, &client->keyboards) {
		if (seat_client_from_keyboard_resource(resource) == NULL) {
			continue;
		}
		wl_keyboard_send_modifiers(resource, serial, modifiers->depressed,
			modifiers->latched, modifiers->locked, modifiers->group);
	}
}
