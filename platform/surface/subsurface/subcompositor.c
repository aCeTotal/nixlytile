#include <stdlib.h>
#include <wayland-server-protocol.h>
#include "platform/surface/subsurface/subsurface.h"

#define SUBCOMPOSITOR_VERSION 1

struct nx_subcompositor {
	struct wl_global *global;
	struct wl_listener display_destroy;
};

static void subcompositor_handle_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct wl_subcompositor_interface subcompositor_impl = {
	.destroy = subcompositor_handle_destroy,
	.get_subsurface = subcompositor_get_subsurface,
};

static void subcompositor_bind(struct wl_client *client, void *data,
		uint32_t version, uint32_t id) {
	struct nx_subcompositor *subcompositor = data;
	struct wl_resource *resource =
		wl_resource_create(client, &wl_subcompositor_interface, 1, id);
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &subcompositor_impl,
		subcompositor, NULL);
}

static void subcompositor_handle_display_destroy(
		struct wl_listener *listener, void *data) {
	struct nx_subcompositor *subcompositor =
		wl_container_of(listener, subcompositor, display_destroy);
	wl_list_remove(&subcompositor->display_destroy.link);
	wl_global_destroy(subcompositor->global);
	free(subcompositor);
}

struct nx_subcompositor *nx_subcompositor_create(struct wl_display *display) {
	struct nx_subcompositor *subcompositor = calloc(1, sizeof(*subcompositor));
	if (!subcompositor) {
		return NULL;
	}

	subcompositor->global = wl_global_create(display,
		&wl_subcompositor_interface, SUBCOMPOSITOR_VERSION,
		subcompositor, subcompositor_bind);
	if (!subcompositor->global) {
		free(subcompositor);
		return NULL;
	}

	subcompositor->display_destroy.notify = subcompositor_handle_display_destroy;
	wl_display_add_destroy_listener(display, &subcompositor->display_destroy);

	return subcompositor;
}
