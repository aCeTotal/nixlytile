#include <assert.h>
#include <stdlib.h>
#include <wayland-server-core.h>
#include "platform/clipboard/data_device/data_device.h"
#include "platform/input/seat/client.h"
#include "platform/surface/compositor.h"

static void touch_release(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct wl_touch_interface touch_impl = {
	.release = touch_release,
};

static void create_inert_touch(struct wl_client *client, uint32_t version,
		uint32_t id) {
	struct wl_resource *resource =
		wl_resource_create(client, &wl_touch_interface, version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &touch_impl, NULL, NULL);
}

static void seat_handle_get_pointer(struct wl_client *client,
		struct wl_resource *seat_resource, uint32_t id) {
	uint32_t version = wl_resource_get_version(seat_resource);
	struct nx_seat_client *seat_client =
		nx_seat_client_from_resource(seat_resource);
	if (seat_client == NULL) {
		seat_client_create_inert_pointer(client, version, id);
		return;
	}
	if (!(seat_client->seat->accumulated_capabilities & WL_SEAT_CAPABILITY_POINTER)) {
		wl_resource_post_error(seat_resource, WL_SEAT_ERROR_MISSING_CAPABILITY,
			"wl_seat.get_pointer called when no pointer capability has existed");
		return;
	}
	seat_client_create_pointer(seat_client, version, id);
}

static void seat_handle_get_keyboard(struct wl_client *client,
		struct wl_resource *seat_resource, uint32_t id) {
	uint32_t version = wl_resource_get_version(seat_resource);
	struct nx_seat_client *seat_client =
		nx_seat_client_from_resource(seat_resource);
	if (seat_client == NULL) {
		seat_client_create_inert_keyboard(client, version, id);
		return;
	}
	if (!(seat_client->seat->accumulated_capabilities & WL_SEAT_CAPABILITY_KEYBOARD)) {
		wl_resource_post_error(seat_resource, WL_SEAT_ERROR_MISSING_CAPABILITY,
			"wl_seat.get_keyboard called when no keyboard capability has existed");
		return;
	}
	seat_client_create_keyboard(seat_client, version, id);
}

// The seat never offers touch.
static void seat_handle_get_touch(struct wl_client *client,
		struct wl_resource *seat_resource, uint32_t id) {
	if (nx_seat_client_from_resource(seat_resource) == NULL) {
		create_inert_touch(client, wl_resource_get_version(seat_resource), id);
		return;
	}
	wl_resource_post_error(seat_resource, WL_SEAT_ERROR_MISSING_CAPABILITY,
		"wl_seat.get_touch called when no touch capability has existed");
}

static void seat_handle_release(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct wl_seat_interface seat_impl = {
	.get_pointer = seat_handle_get_pointer,
	.get_keyboard = seat_handle_get_keyboard,
	.get_touch = seat_handle_get_touch,
	.release = seat_handle_release,
};

void seat_resource_make_inert(struct wl_resource *resource) {
	wl_list_remove(wl_resource_get_link(resource));
	wl_list_init(wl_resource_get_link(resource));
	wl_resource_set_user_data(resource, NULL);
}

static void make_all_inert(struct wl_list *resources) {
	struct wl_resource *resource, *tmp;
	wl_resource_for_each_safe(resource, tmp, resources) {
		seat_resource_make_inert(resource);
	}
}

void seat_client_destroy(struct nx_seat_client *client) {
	wl_signal_emit_mutable(&client->events.destroy, client);
	assert(wl_list_empty(&client->events.destroy.listener_list));

	struct nx_seat *seat = client->seat;
	if (client == seat->pointer_state.focused_client) {
		seat->pointer_state.focused_client = NULL;
	}
	if (client == seat->keyboard_state.focused_client) {
		seat->keyboard_state.focused_client = NULL;
	}
	if (seat->drag != NULL && client == seat->drag->seat_client) {
		seat->drag->seat_client = NULL;
	}

	make_all_inert(&client->pointers);
	make_all_inert(&client->keyboards);
	make_all_inert(&client->data_devices);
	make_all_inert(&client->resources);

	wl_list_remove(&client->link);
	free(client);
}

static void seat_client_handle_resource_destroy(struct wl_resource *resource) {
	struct nx_seat_client *client = nx_seat_client_from_resource(resource);
	if (client == NULL) {
		return;
	}

	wl_list_remove(wl_resource_get_link(resource));
	if (wl_list_empty(&client->resources)) {
		seat_client_destroy(client);
	}
}

static bool surface_belongs_to(struct nx_surface *surface,
		struct wl_client *client) {
	return surface != NULL && wl_resource_get_client(surface->resource) == client;
}

static struct nx_seat_client *seat_client_create(struct nx_seat *seat,
		struct wl_client *client) {
	struct nx_seat_client *seat_client = calloc(1, sizeof(*seat_client));
	if (seat_client == NULL) {
		return NULL;
	}

	seat_client->client = client;
	seat_client->seat = seat;
	wl_list_init(&seat_client->resources);
	wl_list_init(&seat_client->pointers);
	wl_list_init(&seat_client->keyboards);
	wl_list_init(&seat_client->data_devices);
	wl_signal_init(&seat_client->events.destroy);
	wl_list_insert(&seat->clients, &seat_client->link);

	if (surface_belongs_to(seat->pointer_state.focused_surface, client)) {
		seat->pointer_state.focused_client = seat_client;
	}
	if (surface_belongs_to(seat->keyboard_state.focused_surface, client)) {
		seat->keyboard_state.focused_client = seat_client;
	}
	return seat_client;
}

void seat_bind(struct wl_client *client, void *data, uint32_t version,
		uint32_t id) {
	struct nx_seat *seat = data; // NULL while the global dies
	struct wl_resource *resource =
		wl_resource_create(client, &wl_seat_interface, version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &seat_impl, NULL,
		seat_client_handle_resource_destroy);
	wl_list_init(wl_resource_get_link(resource));
	if (seat == NULL) {
		return;
	}

	struct nx_seat_client *seat_client = nx_seat_client_for_wl_client(seat, client);
	if (seat_client == NULL) {
		seat_client = seat_client_create(seat, client);
	}
	if (seat_client == NULL) {
		wl_resource_destroy(resource);
		wl_client_post_no_memory(client);
		return;
	}

	wl_resource_set_user_data(resource, seat_client);
	wl_list_insert(&seat_client->resources, wl_resource_get_link(resource));
	if (version >= WL_SEAT_NAME_SINCE_VERSION) {
		wl_seat_send_name(resource, seat->name);
	}
	wl_seat_send_capabilities(resource, seat->capabilities);
}

struct nx_seat_client *nx_seat_client_for_wl_client(struct nx_seat *seat,
		struct wl_client *wl_client) {
	struct nx_seat_client *seat_client;
	wl_list_for_each(seat_client, &seat->clients, link) {
		if (seat_client->client == wl_client) {
			return seat_client;
		}
	}
	return NULL;
}

struct nx_seat_client *nx_seat_client_from_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &wl_seat_interface, &seat_impl));
	return wl_resource_get_user_data(resource);
}
