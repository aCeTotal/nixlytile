#include <assert.h>
#include <stdlib.h>
#include "fifo-v1-protocol.h"
#include "platform/surface/fifo.h"
#include "platform/surface/sync/fifo_manager.h"

#define FIFO_V1_VERSION 1

struct nx_fifo_manager_v1 {
	struct wl_global *global;
	struct wl_listener display_destroy;
};

static void manager_handle_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct wp_fifo_manager_v1_interface manager_impl = {
	.destroy = manager_handle_destroy,
	.get_fifo = fifo_manager_get_fifo,
};

static void manager_bind(struct wl_client *client, void *data,
		uint32_t version, uint32_t id) {
	struct wl_resource *resource = wl_resource_create(client,
		&wp_fifo_manager_v1_interface, version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &manager_impl, NULL, NULL);
}

static void handle_display_destroy(struct wl_listener *listener, void *data) {
	struct nx_fifo_manager_v1 *manager =
		wl_container_of(listener, manager, display_destroy);
	wl_list_remove(&manager->display_destroy.link);
	wl_global_destroy(manager->global);
	free(manager);
}

struct nx_fifo_manager_v1 *nx_fifo_manager_v1_create(
		struct wl_display *display, uint32_t version) {
	assert(version <= FIFO_V1_VERSION);

	struct nx_fifo_manager_v1 *manager = calloc(1, sizeof(*manager));
	if (manager == NULL) {
		return NULL;
	}

	manager->global = wl_global_create(display, &wp_fifo_manager_v1_interface,
		version, NULL, manager_bind);
	if (manager->global == NULL) {
		free(manager);
		return NULL;
	}

	manager->display_destroy.notify = handle_display_destroy;
	wl_display_add_destroy_listener(display, &manager->display_destroy);

	return manager;
}
