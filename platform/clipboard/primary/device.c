#include <assert.h>
#include <stdlib.h>
#include "platform/clipboard/primary/device.h"
#include "primary-selection-unstable-v1-protocol.h"

static const struct zwp_primary_selection_device_v1_interface device_impl;

struct primary_device *primary_device_from_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource,
		&zwp_primary_selection_device_v1_interface, &device_impl));
	return wl_resource_get_user_data(resource);
}

static void device_handle_set_selection(struct wl_client *client,
		struct wl_resource *resource, struct wl_resource *source_resource,
		uint32_t serial) {
	struct primary_device *device = primary_device_from_resource(resource);
	if (device == NULL) {
		return;
	}

	struct nx_primary_selection_source *source =
		primary_source_finalize(source_resource);
	struct nx_seat_client *seat_client =
		nx_seat_client_for_wl_client(device->seat, client);

	nx_seat_request_set_primary_selection(device->seat, seat_client, source,
		serial);
}

static void device_handle_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct zwp_primary_selection_device_v1_interface device_impl = {
	.set_selection = device_handle_set_selection,
	.destroy = device_handle_destroy,
};

static void device_handle_resource_destroy(struct wl_resource *resource) {
	wl_list_remove(wl_resource_get_link(resource));
}

static void device_resource_send_selection(struct wl_resource *resource,
		struct nx_primary_selection_source *source) {
	assert(primary_device_from_resource(resource) != NULL);

	if (source == NULL) {
		zwp_primary_selection_device_v1_send_selection(resource, NULL);
		return;
	}
	primary_offer_create(resource, source);
}

static void device_send_selection(struct primary_device *device) {
	struct nx_seat_client *seat_client =
		device->seat->keyboard_state.focused_client;
	if (seat_client == NULL) {
		return;
	}

	struct wl_resource *resource;
	wl_resource_for_each(resource, &device->resources) {
		if (wl_resource_get_client(resource) == seat_client->client) {
			device_resource_send_selection(resource,
				device->seat->primary_selection_source);
		}
	}
}

static void device_handle_seat_destroy(struct wl_listener *listener,
		void *data) {
	struct primary_device *device =
		wl_container_of(listener, device, seat_destroy);
	primary_device_destroy(device);
}

static void device_handle_seat_focus_change(struct wl_listener *listener,
		void *data) {
	struct primary_device *device =
		wl_container_of(listener, device, seat_focus_change);
	device_send_selection(device);
}

static void device_handle_seat_set_primary_selection(
		struct wl_listener *listener, void *data) {
	struct primary_device *device =
		wl_container_of(listener, device, seat_set_primary_selection);

	struct wl_resource *resource, *tmp;
	wl_resource_for_each_safe(resource, tmp, &device->offers) {
		primary_offer_destroy(resource);
	}

	device_send_selection(device);
}

static struct primary_device *get_or_create_device(
		struct nx_primary_selection_v1_device_manager *manager,
		struct nx_seat *seat) {
	struct primary_device *device;
	wl_list_for_each(device, &manager->devices, link) {
		if (device->seat == seat) {
			return device;
		}
	}

	device = calloc(1, sizeof(*device));
	if (device == NULL) {
		return NULL;
	}
	device->seat = seat;

	wl_list_init(&device->resources);
	wl_list_insert(&manager->devices, &device->link);

	wl_list_init(&device->offers);

	device->seat_destroy.notify = device_handle_seat_destroy;
	wl_signal_add(&seat->events.destroy, &device->seat_destroy);

	device->seat_focus_change.notify = device_handle_seat_focus_change;
	wl_signal_add(&seat->keyboard_state.events.focus_change,
		&device->seat_focus_change);

	device->seat_set_primary_selection.notify =
		device_handle_seat_set_primary_selection;
	wl_signal_add(&seat->events.set_primary_selection,
		&device->seat_set_primary_selection);

	return device;
}

void primary_device_destroy(struct primary_device *device) {
	if (device == NULL) {
		return;
	}
	wl_list_remove(&device->link);
	wl_list_remove(&device->seat_destroy.link);
	wl_list_remove(&device->seat_focus_change.link);
	wl_list_remove(&device->seat_set_primary_selection.link);

	struct wl_resource *resource, *resource_tmp;
	wl_resource_for_each_safe(resource, resource_tmp, &device->offers) {
		primary_offer_destroy(resource);
	}
	wl_resource_for_each_safe(resource, resource_tmp, &device->resources) {
		wl_resource_set_user_data(resource, NULL);

		struct wl_list *link = wl_resource_get_link(resource);
		wl_list_remove(link);
		wl_list_init(link);
	}
	free(device);
}

void primary_device_bind(struct nx_primary_selection_v1_device_manager *manager,
		struct wl_resource *manager_resource, uint32_t id,
		struct wl_resource *seat_resource) {
	struct nx_seat_client *seat_client =
		nx_seat_client_from_resource(seat_resource);

	struct wl_resource *resource = wl_resource_create(
		wl_resource_get_client(manager_resource),
		&zwp_primary_selection_device_v1_interface,
		wl_resource_get_version(manager_resource), id);
	if (resource == NULL) {
		wl_resource_post_no_memory(manager_resource);
		return;
	}
	wl_resource_set_implementation(resource, &device_impl, NULL,
		device_handle_resource_destroy);
	wl_list_init(wl_resource_get_link(resource));
	if (seat_client == NULL) {
		return;
	}

	struct primary_device *device = get_or_create_device(manager,
		seat_client->seat);
	if (device == NULL) {
		wl_resource_destroy(resource);
		wl_resource_post_no_memory(manager_resource);
		return;
	}
	wl_resource_set_user_data(resource, device);
	wl_list_insert(&device->resources, wl_resource_get_link(resource));

	if (device->seat->keyboard_state.focused_client == seat_client) {
		device_resource_send_selection(resource,
			device->seat->primary_selection_source);
	}
}
