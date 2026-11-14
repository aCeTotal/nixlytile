#include <assert.h>
#include <stdlib.h>
#include "platform/clipboard/primary/device.h"
#include "primary-selection-unstable-v1-protocol.h"

#define DEVICE_MANAGER_VERSION 1

static const struct zwp_primary_selection_device_manager_v1_interface
	manager_impl;

static struct nx_primary_selection_v1_device_manager *manager_from_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource,
		&zwp_primary_selection_device_manager_v1_interface, &manager_impl));
	return wl_resource_get_user_data(resource);
}

static void manager_handle_create_source(struct wl_client *client,
		struct wl_resource *manager_resource, uint32_t id) {
	primary_source_create(client, manager_resource, id);
}

static void manager_handle_get_device(struct wl_client *client,
		struct wl_resource *manager_resource, uint32_t id,
		struct wl_resource *seat_resource) {
	primary_device_bind(manager_from_resource(manager_resource),
		manager_resource, id, seat_resource);
}

static void manager_handle_destroy(struct wl_client *client,
		struct wl_resource *manager_resource) {
	wl_resource_destroy(manager_resource);
}

static const struct zwp_primary_selection_device_manager_v1_interface
		manager_impl = {
	.create_source = manager_handle_create_source,
	.get_device = manager_handle_get_device,
	.destroy = manager_handle_destroy,
};

static void manager_bind(struct wl_client *client, void *data,
		uint32_t version, uint32_t id) {
	struct wl_resource *resource = wl_resource_create(client,
		&zwp_primary_selection_device_manager_v1_interface, version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &manager_impl, data, NULL);
}

static void handle_display_destroy(struct wl_listener *listener, void *data) {
	struct nx_primary_selection_v1_device_manager *manager =
		wl_container_of(listener, manager, display_destroy);

	struct primary_device *device, *tmp;
	wl_list_for_each_safe(device, tmp, &manager->devices, link) {
		primary_device_destroy(device);
	}

	wl_list_remove(&manager->display_destroy.link);
	wl_global_destroy(manager->global);
	free(manager);
}

struct nx_primary_selection_v1_device_manager *
		nx_primary_selection_v1_device_manager_create(
		struct wl_display *display) {
	struct nx_primary_selection_v1_device_manager *manager =
		calloc(1, sizeof(*manager));
	if (manager == NULL) {
		return NULL;
	}
	manager->global = wl_global_create(display,
		&zwp_primary_selection_device_manager_v1_interface,
		DEVICE_MANAGER_VERSION, manager, manager_bind);
	if (manager->global == NULL) {
		free(manager);
		return NULL;
	}

	wl_list_init(&manager->devices);

	manager->display_destroy.notify = handle_display_destroy;
	wl_display_add_destroy_listener(display, &manager->display_destroy);

	return manager;
}
