#include <assert.h>
#include <stdlib.h>
#include <unistd.h>
#include "platform/clipboard/control/data_control.h"
#include "ext-data-control-v1-protocol.h"
#include "wlr-data-control-unstable-v1-protocol.h"

// Always its device's current offer.
struct control_offer {
	struct wl_resource *resource;
	struct control_device *device;
	bool is_primary;
};

static const struct ext_data_control_offer_v1_interface offer_impl;

static struct control_offer *offer_from_resource(struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource,
			&ext_data_control_offer_v1_interface, &offer_impl) ||
		wl_resource_instance_of(resource,
			&zwlr_data_control_offer_v1_interface, &offer_impl));
	return wl_resource_get_user_data(resource);
}

static void offer_destroy(struct control_offer *offer) {
	if (offer == NULL) {
		return;
	}

	if (offer->is_primary) {
		offer->device->primary_selection_offer_resource = NULL;
	} else {
		offer->device->selection_offer_resource = NULL;
	}

	wl_resource_set_user_data(offer->resource, NULL);
	free(offer);
}

void control_offer_destroy(struct wl_resource *resource) {
	if (resource == NULL) {
		return;
	}
	offer_destroy(offer_from_resource(resource));
}

static void offer_handle_receive(struct wl_client *client,
		struct wl_resource *resource, const char *mime_type, int fd) {
	struct control_offer *offer = offer_from_resource(resource);
	if (offer == NULL) {
		close(fd);
		return;
	}

	struct nx_seat *seat = offer->device->seat;
	if (offer->is_primary && seat->primary_selection_source != NULL) {
		nx_primary_selection_source_send(seat->primary_selection_source,
			mime_type, fd);
		return;
	}
	if (!offer->is_primary && seat->selection_source != NULL) {
		nx_data_source_send(seat->selection_source, mime_type, fd);
		return;
	}
	close(fd);
}

static void offer_handle_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct ext_data_control_offer_v1_interface offer_impl = {
	.receive = offer_handle_receive,
	.destroy = offer_handle_destroy,
};

static void offer_handle_resource_destroy(struct wl_resource *resource) {
	offer_destroy(offer_from_resource(resource));
}

struct wl_resource *control_offer_create(struct control_device *device,
		struct wl_array *mime_types, bool is_primary) {
	struct wl_client *client = wl_resource_get_client(device->resource);

	struct control_offer *offer = calloc(1, sizeof(*offer));
	if (offer == NULL) {
		wl_client_post_no_memory(client);
		return NULL;
	}

	offer->device = device;
	offer->is_primary = is_primary;

	uint32_t version = wl_resource_get_version(device->resource);
	struct wl_resource *resource = wl_resource_create(client,
		device->protocol->offer, version, 0);
	if (resource == NULL) {
		free(offer);
		return NULL;
	}

	offer->resource = resource;

	wl_resource_set_implementation(resource, &offer_impl, offer,
		offer_handle_resource_destroy);

	ext_data_control_device_v1_send_data_offer(device->resource, resource);

	char **p;
	wl_array_for_each(p, mime_types) {
		ext_data_control_offer_v1_send_offer(resource, *p);
	}

	return resource;
}
