#include <assert.h>
#include <wayland-server-core.h>
#include "platform/clipboard/data_device/data_device.h"
#include "platform/clipboard/data_device/drag.h"

static const struct wl_data_device_interface data_device_impl;

struct nx_seat_client *seat_client_from_data_device_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &wl_data_device_interface,
		&data_device_impl));
	return wl_resource_get_user_data(resource);
}

static struct nx_client_data_source *client_source_from_optional(
		struct wl_resource *source_resource) {
	if (source_resource == NULL) {
		return NULL;
	}
	return client_data_source_from_resource(source_resource);
}

static void data_device_set_selection(struct wl_client *client,
		struct wl_resource *device_resource,
		struct wl_resource *source_resource, uint32_t serial) {
	struct nx_seat_client *seat_client =
		seat_client_from_data_device_resource(device_resource);
	if (seat_client == NULL) {
		return;
	}

	struct nx_client_data_source *source =
		client_source_from_optional(source_resource);
	struct nx_data_source *nx_source = NULL;
	if (source != NULL) {
		source->finalized = true;
		nx_source = &source->source;
	}

	nx_seat_request_set_selection(seat_client->seat, seat_client, nx_source,
		serial);
}

static void data_device_start_drag(struct wl_client *client,
		struct wl_resource *device_resource,
		struct wl_resource *source_resource,
		struct wl_resource *origin_resource, struct wl_resource *icon_resource,
		uint32_t serial) {
	struct nx_seat_client *seat_client =
		seat_client_from_data_device_resource(device_resource);
	if (seat_client == NULL) {
		return;
	}

	struct nx_surface *origin = nx_surface_from_resource(origin_resource);
	struct nx_client_data_source *source =
		client_source_from_optional(source_resource);

	struct nx_surface *icon = icon_resource != NULL ?
		nx_surface_from_resource(icon_resource) : NULL;
	if (icon != NULL && !nx_surface_set_role(icon, &drag_icon_surface_role,
			icon_resource, WL_DATA_DEVICE_ERROR_ROLE)) {
		return;
	}

	struct nx_drag *drag = drag_create(seat_client,
		source != NULL ? &source->source : NULL, icon);
	if (drag == NULL) {
		wl_resource_post_no_memory(device_resource);
		return;
	}

	if (source != NULL) {
		source->finalized = true;
	}

	nx_seat_request_start_drag(seat_client->seat, drag, origin, serial);
}

static void data_device_release(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct wl_data_device_interface data_device_impl = {
	.start_drag = data_device_start_drag,
	.set_selection = data_device_set_selection,
	.release = data_device_release,
};

static void data_device_handle_resource_destroy(struct wl_resource *resource) {
	wl_list_remove(wl_resource_get_link(resource));
	wl_list_init(wl_resource_get_link(resource));
}

void data_device_create(struct wl_resource *manager_resource, uint32_t id,
		struct wl_resource *seat_resource) {
	struct nx_seat_client *seat_client =
		nx_seat_client_from_resource(seat_resource);

	struct wl_resource *resource = wl_resource_create(
		wl_resource_get_client(manager_resource), &wl_data_device_interface,
		wl_resource_get_version(manager_resource), id);
	if (resource == NULL) {
		wl_resource_post_no_memory(manager_resource);
		return;
	}
	wl_resource_set_implementation(resource, &data_device_impl, seat_client,
		data_device_handle_resource_destroy);
	if (seat_client == NULL) {
		wl_list_init(wl_resource_get_link(resource));
		return;
	}
	wl_list_insert(&seat_client->data_devices, wl_resource_get_link(resource));

	if (seat_client->seat->keyboard_state.focused_client == seat_client) {
		data_device_send_selection(resource);
	}
}
