#include <assert.h>
#include <wayland-server-core.h>
#include "platform/input/seat/client.h"
#include "platform/surface/compositor.h"

static const struct wl_pointer_interface pointer_impl;

struct nx_seat_client *nx_seat_client_from_pointer_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &wl_pointer_interface,
		&pointer_impl));
	return wl_resource_get_user_data(resource);
}

void seat_pointer_resource_send_frame(struct wl_resource *resource) {
	if (wl_resource_get_version(resource) >= WL_POINTER_FRAME_SINCE_VERSION) {
		wl_pointer_send_frame(resource);
	}
}

static void cursor_surface_handle_commit(struct nx_surface *surface) {
	pixman_region32_clear(&surface->input_region);
	if (nx_surface_has_buffer(surface)) {
		nx_surface_map(surface);
	}
}

static const struct nx_surface_role cursor_surface_role = {
	.name = "wl_pointer-cursor",
	.no_object = true,
	.commit = cursor_surface_handle_commit,
};

// Posts role error on failure.
static bool cursor_surface_take_role(struct wl_resource *surface_resource) {
	struct nx_surface *surface = nx_surface_from_resource(surface_resource);
	if (!nx_surface_set_role(surface, &cursor_surface_role,
			surface_resource, WL_POINTER_ERROR_ROLE)) {
		return false;
	}
	cursor_surface_handle_commit(surface);
	return true;
}

static void pointer_set_cursor(struct wl_client *client,
		struct wl_resource *pointer_resource, uint32_t serial,
		struct wl_resource *surface_resource,
		int32_t hotspot_x, int32_t hotspot_y) {
	struct nx_seat_client *seat_client =
		nx_seat_client_from_pointer_resource(pointer_resource);
	if (seat_client == NULL) {
		return;
	}
	if (surface_resource != NULL && !cursor_surface_take_role(surface_resource)) {
		return;
	}

	struct nx_seat_pointer_request_set_cursor_event event = {
		.seat_client = seat_client,
		.surface = surface_resource != NULL ?
			nx_surface_from_resource(surface_resource) : NULL,
		.serial = serial,
		.hotspot_x = hotspot_x,
		.hotspot_y = hotspot_y,
	};
	wl_signal_emit_mutable(&seat_client->seat->events.request_set_cursor, &event);
}

static void pointer_release(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct wl_pointer_interface pointer_impl = {
	.set_cursor = pointer_set_cursor,
	.release = pointer_release,
};

void seat_client_create_pointer(struct nx_seat_client *seat_client,
		uint32_t version, uint32_t id) {
	struct wl_resource *resource = wl_resource_create(seat_client->client,
		&wl_pointer_interface, version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(seat_client->client);
		return;
	}
	wl_resource_set_implementation(resource, &pointer_impl, seat_client,
		seat_resource_make_inert);
	wl_list_insert(&seat_client->pointers, wl_resource_get_link(resource));

	struct nx_seat *seat = seat_client->seat;
	if ((seat->capabilities & WL_SEAT_CAPABILITY_POINTER) == 0) {
		wl_resource_set_user_data(resource, NULL);
		return;
	}

	struct nx_seat_pointer_state *state = &seat->pointer_state;
	if (state->focused_client != seat_client || state->focused_surface == NULL) {
		return;
	}
	uint32_t serial = nx_seat_client_next_serial(seat_client);
	wl_pointer_send_enter(resource, serial, state->focused_surface->resource,
		wl_fixed_from_double(state->sx), wl_fixed_from_double(state->sy));
	seat_pointer_resource_send_frame(resource);
}

void seat_client_create_inert_pointer(struct wl_client *client,
		uint32_t version, uint32_t id) {
	struct wl_resource *resource =
		wl_resource_create(client, &wl_pointer_interface, version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &pointer_impl, NULL, NULL);
}
