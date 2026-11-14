#include <assert.h>
#include <unistd.h>
#include "platform/clipboard/primary/device.h"
#include "primary-selection-unstable-v1-protocol.h"

static const struct zwp_primary_selection_offer_v1_interface offer_impl;

static struct primary_device *device_from_offer_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource,
		&zwp_primary_selection_offer_v1_interface, &offer_impl));
	return wl_resource_get_user_data(resource);
}

static void offer_handle_receive(struct wl_client *client,
		struct wl_resource *resource, const char *mime_type, int32_t fd) {
	struct primary_device *device = device_from_offer_resource(resource);
	if (device == NULL || device->seat->primary_selection_source == NULL) {
		close(fd);
		return;
	}

	nx_primary_selection_source_send(device->seat->primary_selection_source,
		mime_type, fd);
}

static void offer_handle_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct zwp_primary_selection_offer_v1_interface offer_impl = {
	.receive = offer_handle_receive,
	.destroy = offer_handle_destroy,
};

static void offer_handle_resource_destroy(struct wl_resource *resource) {
	wl_list_remove(wl_resource_get_link(resource));
}

void primary_offer_create(struct wl_resource *device_resource,
		struct nx_primary_selection_source *source) {
	struct primary_device *device =
		primary_device_from_resource(device_resource);
	assert(device != NULL);

	struct wl_client *client = wl_resource_get_client(device_resource);
	uint32_t version = wl_resource_get_version(device_resource);
	struct wl_resource *resource = wl_resource_create(client,
		&zwp_primary_selection_offer_v1_interface, version, 0);
	if (resource == NULL) {
		wl_resource_post_no_memory(device_resource);
		return;
	}
	wl_resource_set_implementation(resource, &offer_impl, device,
		offer_handle_resource_destroy);

	wl_list_insert(&device->offers, wl_resource_get_link(resource));

	zwp_primary_selection_device_v1_send_data_offer(device_resource, resource);

	char **p;
	wl_array_for_each(p, &source->mime_types) {
		zwp_primary_selection_offer_v1_send_offer(resource, *p);
	}

	zwp_primary_selection_device_v1_send_selection(device_resource, resource);
}

void primary_offer_destroy(struct wl_resource *resource) {
	if (device_from_offer_resource(resource) == NULL) {
		return;
	}

	wl_resource_set_user_data(resource, NULL);

	struct wl_list *link = wl_resource_get_link(resource);
	wl_list_remove(link);
	wl_list_init(link);
}
