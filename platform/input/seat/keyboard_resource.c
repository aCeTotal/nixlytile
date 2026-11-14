#include <assert.h>
#include <wayland-server-core.h>
#include "platform/input/seat/client.h"
#include "platform/surface/compositor.h"

static void keyboard_release(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct wl_keyboard_interface keyboard_impl = {
	.release = keyboard_release,
};

struct nx_seat_client *seat_client_from_keyboard_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &wl_keyboard_interface,
		&keyboard_impl));
	return wl_resource_get_user_data(resource);
}

void seat_client_create_keyboard(struct nx_seat_client *seat_client,
		uint32_t version, uint32_t id) {
	struct wl_resource *resource = wl_resource_create(seat_client->client,
		&wl_keyboard_interface, version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(seat_client->client);
		return;
	}
	wl_resource_set_implementation(resource, &keyboard_impl, seat_client,
		seat_resource_make_inert);
	wl_list_insert(&seat_client->keyboards, wl_resource_get_link(resource));

	struct nx_seat *seat = seat_client->seat;
	if ((seat->capabilities & WL_SEAT_CAPABILITY_KEYBOARD) == 0) {
		wl_resource_set_user_data(resource, NULL);
		return;
	}

	struct nx_seat_keyboard_state *state = &seat->keyboard_state;
	struct nx_keyboard *keyboard = state->keyboard;
	if (keyboard == NULL) {
		return;
	}
	seat_client_send_keymap(seat_client, keyboard);
	seat_client_send_repeat_info(seat_client, keyboard);

	if (state->focused_client != seat_client || state->focused_surface == NULL) {
		return;
	}
	struct wl_array keys = {
		.data = keyboard->keycodes,
		.size = keyboard->num_keycodes * sizeof(keyboard->keycodes[0]),
	};
	uint32_t serial = nx_seat_client_next_serial(seat_client);
	wl_keyboard_send_enter(resource, serial, state->focused_surface->resource,
		&keys);
	nx_seat_keyboard_send_modifiers(seat, &keyboard->modifiers);
}

void seat_client_create_inert_keyboard(struct wl_client *client,
		uint32_t version, uint32_t id) {
	struct wl_resource *resource =
		wl_resource_create(client, &wl_keyboard_interface, version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &keyboard_impl, NULL, NULL);
}
