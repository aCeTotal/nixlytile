#include <assert.h>
#include <stdlib.h>
#include "platform/clipboard/control/data_control.h"
#include "ext-data-control-v1-protocol.h"
#include "wlr-data-control-unstable-v1-protocol.h"

static const struct ext_data_control_device_v1_interface device_impl;

static struct control_device *device_from_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource,
			&ext_data_control_device_v1_interface, &device_impl) ||
		wl_resource_instance_of(resource,
			&zwlr_data_control_device_v1_interface, &device_impl));
	return wl_resource_get_user_data(resource);
}

static struct control_source *source_from_optional(
		struct wl_resource *source_resource) {
	if (source_resource == NULL) {
		return NULL;
	}
	return control_source_from_resource(source_resource);
}

// Posts used-source error when false.
static bool source_is_unused(struct wl_resource *device_resource,
		struct control_source *source) {
	if (source->active_source == NULL &&
			source->active_primary_source == NULL) {
		return true;
	}

	wl_resource_post_error(device_resource,
		EXT_DATA_CONTROL_DEVICE_V1_ERROR_USED_SOURCE,
		"cannot use a data source in set_selection or "
		"set_primary_selection more than once");
	return false;
}

static void device_handle_set_selection(struct wl_client *client,
		struct wl_resource *resource, struct wl_resource *source_resource) {
	struct control_device *device = device_from_resource(resource);
	if (device == NULL) {
		return;
	}

	struct control_source *source = source_from_optional(source_resource);
	if (source == NULL) {
		nx_seat_request_set_selection(device->seat, NULL, NULL,
			wl_display_next_serial(device->seat->display));
		return;
	}
	if (!source_is_unused(resource, source)) {
		return;
	}

	struct nx_data_source *selection = control_source_set_selection(source);
	if (selection == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	nx_seat_request_set_selection(device->seat, NULL, selection,
		wl_display_next_serial(device->seat->display));
}

static void device_handle_set_primary_selection(struct wl_client *client,
		struct wl_resource *resource, struct wl_resource *source_resource) {
	struct control_device *device = device_from_resource(resource);
	if (device == NULL) {
		return;
	}

	struct control_source *source = source_from_optional(source_resource);
	if (source == NULL) {
		nx_seat_request_set_primary_selection(device->seat, NULL, NULL,
			wl_display_next_serial(device->seat->display));
		return;
	}
	if (!source_is_unused(resource, source)) {
		return;
	}

	struct nx_primary_selection_source *selection =
		control_source_set_primary_selection(source);
	if (selection == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	nx_seat_request_set_primary_selection(device->seat, NULL, selection,
		wl_display_next_serial(device->seat->display));
}

static void device_handle_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct ext_data_control_device_v1_interface device_impl = {
	.set_selection = device_handle_set_selection,
	.set_primary_selection = device_handle_set_primary_selection,
	.destroy = device_handle_destroy,
};

static void device_send_selection(struct control_device *device) {
	control_offer_destroy(device->selection_offer_resource);

	struct nx_data_source *source = device->seat->selection_source;
	if (source == NULL) {
		ext_data_control_device_v1_send_selection(device->resource, NULL);
		return;
	}

	device->selection_offer_resource =
		control_offer_create(device, &source->mime_types, false);
	if (device->selection_offer_resource == NULL) {
		wl_resource_post_no_memory(device->resource);
		return;
	}
	ext_data_control_device_v1_send_selection(device->resource,
		device->selection_offer_resource);
}

static void device_send_primary_selection(struct control_device *device) {
	if (wl_resource_get_version(device->resource) <
			device->protocol->primary_selection_since) {
		return;
	}

	control_offer_destroy(device->primary_selection_offer_resource);

	struct nx_primary_selection_source *source =
		device->seat->primary_selection_source;
	if (source == NULL) {
		ext_data_control_device_v1_send_primary_selection(device->resource,
			NULL);
		return;
	}

	device->primary_selection_offer_resource =
		control_offer_create(device, &source->mime_types, true);
	if (device->primary_selection_offer_resource == NULL) {
		wl_resource_post_no_memory(device->resource);
		return;
	}
	ext_data_control_device_v1_send_primary_selection(device->resource,
		device->primary_selection_offer_resource);
}

static void device_destroy(struct control_device *device) {
	if (device == NULL) {
		return;
	}
	ext_data_control_device_v1_send_finished(device->resource);
	wl_resource_set_user_data(device->resource, NULL);
	control_offer_destroy(device->selection_offer_resource);
	control_offer_destroy(device->primary_selection_offer_resource);
	wl_list_remove(&device->seat_destroy.link);
	wl_list_remove(&device->seat_set_selection.link);
	wl_list_remove(&device->seat_set_primary_selection.link);
	free(device);
}

static void device_handle_resource_destroy(struct wl_resource *resource) {
	device_destroy(device_from_resource(resource));
}

static void device_handle_seat_destroy(struct wl_listener *listener,
		void *data) {
	struct control_device *device =
		wl_container_of(listener, device, seat_destroy);
	device_destroy(device);
}

static void device_handle_seat_set_selection(struct wl_listener *listener,
		void *data) {
	struct control_device *device =
		wl_container_of(listener, device, seat_set_selection);
	device_send_selection(device);
}

static void device_handle_seat_set_primary_selection(
		struct wl_listener *listener, void *data) {
	struct control_device *device =
		wl_container_of(listener, device, seat_set_primary_selection);
	device_send_primary_selection(device);
}

void control_device_create(const struct control_protocol *protocol,
		struct wl_resource *manager_resource, uint32_t id,
		struct wl_resource *seat_resource) {
	struct nx_seat_client *seat_client =
		nx_seat_client_from_resource(seat_resource);

	struct wl_resource *resource = wl_resource_create(
		wl_resource_get_client(manager_resource), protocol->device,
		wl_resource_get_version(manager_resource), id);
	if (resource == NULL) {
		wl_resource_post_no_memory(manager_resource);
		return;
	}
	wl_resource_set_implementation(resource, &device_impl, NULL,
		device_handle_resource_destroy);
	if (seat_client == NULL) {
		return;
	}

	struct control_device *device = calloc(1, sizeof(*device));
	if (device == NULL) {
		wl_resource_post_no_memory(manager_resource);
		return;
	}
	device->protocol = protocol;
	device->seat = seat_client->seat;
	device->resource = resource;
	wl_resource_set_user_data(resource, device);

	device->seat_destroy.notify = device_handle_seat_destroy;
	wl_signal_add(&device->seat->events.destroy, &device->seat_destroy);

	device->seat_set_selection.notify = device_handle_seat_set_selection;
	wl_signal_add(&device->seat->events.set_selection,
		&device->seat_set_selection);

	device->seat_set_primary_selection.notify =
		device_handle_seat_set_primary_selection;
	wl_signal_add(&device->seat->events.set_primary_selection,
		&device->seat_set_primary_selection);

	device_send_selection(device);
	device_send_primary_selection(device);
}
