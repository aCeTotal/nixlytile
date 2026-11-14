#include <stdlib.h>
#include <wayland-server-core.h>
#include "platform/clipboard/data_device/data_device.h"
#include "platform/log/log.h"

#define DATA_DEVICE_MANAGER_VERSION 3

static void manager_handle_create_data_source(struct wl_client *client,
		struct wl_resource *manager_resource, uint32_t id) {
	client_data_source_create(client,
		wl_resource_get_version(manager_resource), id);
}

static void manager_handle_get_data_device(struct wl_client *client,
		struct wl_resource *manager_resource, uint32_t id,
		struct wl_resource *seat_resource) {
	data_device_create(manager_resource, id, seat_resource);
}

static const struct wl_data_device_manager_interface manager_impl = {
	.create_data_source = manager_handle_create_data_source,
	.get_data_device = manager_handle_get_data_device,
};

static void manager_bind(struct wl_client *client, void *data,
		uint32_t version, uint32_t id) {
	struct wl_resource *resource = wl_resource_create(client,
		&wl_data_device_manager_interface, version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &manager_impl, data, NULL);
}

static void handle_display_destroy(struct wl_listener *listener, void *data) {
	struct nx_data_device_manager *manager =
		wl_container_of(listener, manager, display_destroy);
	wl_list_remove(&manager->display_destroy.link);
	wl_global_destroy(manager->global);
	free(manager);
}

struct nx_data_device_manager *nx_data_device_manager_create(
		struct wl_display *display) {
	struct nx_data_device_manager *manager = calloc(1, sizeof(*manager));
	if (manager == NULL) {
		nx_log(NX_ERROR, "could not create data device manager");
		return NULL;
	}

	manager->global = wl_global_create(display,
		&wl_data_device_manager_interface, DATA_DEVICE_MANAGER_VERSION,
		manager, manager_bind);
	if (manager->global == NULL) {
		nx_log(NX_ERROR, "could not create data device manager wl_global");
		free(manager);
		return NULL;
	}

	manager->display_destroy.notify = handle_display_destroy;
	wl_display_add_destroy_listener(display, &manager->display_destroy);

	return manager;
}
