#include <assert.h>
#include <stdlib.h>
#include "platform/output/layout.h"
#include "platform/output/xdg_output.h"
#include "xdg-output-unstable-v1-protocol.h"

#define OUTPUT_MANAGER_VERSION 3
#define OUTPUT_DONE_DEPRECATED_SINCE_VERSION 3
#define OUTPUT_DESCRIPTION_MUTABLE_SINCE_VERSION 3

struct nx_xdg_output_v1 {
	struct wl_list resources;
	struct wl_list link; // nx_xdg_output_manager_v1.outputs

	struct nx_output_layout_output *layout_output;

	int32_t x, y;
	int32_t width, height;

	struct wl_listener destroy;
	struct wl_listener description;
};

struct nx_xdg_output_manager_v1 {
	struct nx_output_layout *layout;
	struct wl_list outputs; // nx_xdg_output_v1.link

	struct wl_listener display_destroy;
	struct wl_listener layout_add;
	struct wl_listener layout_change;
	struct wl_listener layout_destroy;
};

static void output_handle_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct zxdg_output_v1_interface output_implementation = {
	.destroy = output_handle_destroy,
};

static void output_handle_resource_destroy(struct wl_resource *resource) {
	wl_list_remove(wl_resource_get_link(resource));
}

static void output_send_details(struct nx_xdg_output_v1 *xdg_output,
		struct wl_resource *resource) {
	zxdg_output_v1_send_logical_position(resource,
		xdg_output->x, xdg_output->y);
	zxdg_output_v1_send_logical_size(resource,
		xdg_output->width, xdg_output->height);
	if (wl_resource_get_version(resource) < OUTPUT_DONE_DEPRECATED_SINCE_VERSION) {
		zxdg_output_v1_send_done(resource);
	}
}

static void output_update(struct nx_xdg_output_v1 *xdg_output) {
	struct nx_output_layout_output *layout_output = xdg_output->layout_output;
	int width, height;
	nx_output_effective_resolution(layout_output->output, &width, &height);
	if (layout_output->x == xdg_output->x && layout_output->y == xdg_output->y &&
			width == xdg_output->width && height == xdg_output->height) {
		return;
	}

	xdg_output->x = layout_output->x;
	xdg_output->y = layout_output->y;
	xdg_output->width = width;
	xdg_output->height = height;

	struct wl_resource *resource;
	wl_resource_for_each(resource, &xdg_output->resources) {
		output_send_details(xdg_output, resource);
	}
	nx_output_schedule_done(layout_output->output);
}

static void output_destroy(struct nx_xdg_output_v1 *output) {
	struct wl_resource *resource, *tmp;
	wl_resource_for_each_safe(resource, tmp, &output->resources) {
		wl_list_remove(wl_resource_get_link(resource));
		wl_list_init(wl_resource_get_link(resource));
	}
	wl_list_remove(&output->destroy.link);
	wl_list_remove(&output->description.link);
	wl_list_remove(&output->link);
	free(output);
}

static void output_manager_handle_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct zxdg_output_manager_v1_interface output_manager_implementation;

static struct nx_xdg_output_v1 *manager_find_output(
		struct nx_xdg_output_manager_v1 *manager,
		struct nx_output_layout_output *layout_output) {
	struct nx_xdg_output_v1 *xdg_output;
	wl_list_for_each(xdg_output, &manager->outputs, link) {
		if (xdg_output->layout_output == layout_output) {
			return xdg_output;
		}
	}
	return NULL;
}

// Name and description go out once.
static void send_identity(struct nx_output *output, struct wl_resource *resource) {
	uint32_t version = wl_resource_get_version(resource);
	if (version >= ZXDG_OUTPUT_V1_NAME_SINCE_VERSION) {
		zxdg_output_v1_send_name(resource, output->name);
	}
	if (version >= ZXDG_OUTPUT_V1_DESCRIPTION_SINCE_VERSION &&
			output->description != NULL) {
		zxdg_output_v1_send_description(resource, output->description);
	}
}

static void output_manager_handle_get_xdg_output(struct wl_client *client,
		struct wl_resource *resource, uint32_t id,
		struct wl_resource *output_resource) {
	assert(wl_resource_instance_of(resource, &zxdg_output_manager_v1_interface,
		&output_manager_implementation));

	struct nx_xdg_output_manager_v1 *manager =
		wl_resource_get_user_data(resource);
	struct nx_output *output = nx_output_from_resource(output_resource);

	struct wl_resource *xdg_output_resource = wl_resource_create(client,
		&zxdg_output_v1_interface, wl_resource_get_version(resource), id);
	if (!xdg_output_resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(xdg_output_resource, &output_implementation,
		NULL, output_handle_resource_destroy);

	if (output == NULL) {
		wl_list_init(wl_resource_get_link(xdg_output_resource));
		return;
	}

	struct nx_output_layout_output *layout_output =
		nx_output_layout_get(manager->layout, output);
	assert(layout_output);
	struct nx_xdg_output_v1 *xdg_output = manager_find_output(manager, layout_output);
	assert(xdg_output);

	wl_list_insert(&xdg_output->resources,
		wl_resource_get_link(xdg_output_resource));
	send_identity(output, xdg_output_resource);
	output_send_details(xdg_output, xdg_output_resource);

	uint32_t xdg_version = wl_resource_get_version(xdg_output_resource);
	uint32_t wl_version = wl_resource_get_version(output_resource);
	if (wl_version >= WL_OUTPUT_DONE_SINCE_VERSION &&
			xdg_version >= OUTPUT_DONE_DEPRECATED_SINCE_VERSION) {
		wl_output_send_done(output_resource);
	}
}

static const struct zxdg_output_manager_v1_interface
		output_manager_implementation = {
	.destroy = output_manager_handle_destroy,
	.get_xdg_output = output_manager_handle_get_xdg_output,
};

static void output_manager_bind(struct wl_client *wl_client, void *data,
		uint32_t version, uint32_t id) {
	struct nx_xdg_output_manager_v1 *manager = data;

	struct wl_resource *resource = wl_resource_create(wl_client,
		&zxdg_output_manager_v1_interface, version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(wl_client);
		return;
	}
	wl_resource_set_implementation(resource, &output_manager_implementation,
		manager, NULL);
}

static void handle_output_destroy(struct wl_listener *listener, void *data) {
	struct nx_xdg_output_v1 *output = wl_container_of(listener, output, destroy);
	output_destroy(output);
}

static void handle_output_description(struct wl_listener *listener,
		void *data) {
	struct nx_xdg_output_v1 *xdg_output =
		wl_container_of(listener, xdg_output, description);
	struct nx_output *output = xdg_output->layout_output->output;
	if (output->description == NULL) {
		return;
	}

	struct wl_resource *resource;
	wl_resource_for_each(resource, &xdg_output->resources) {
		if (wl_resource_get_version(resource) >=
				OUTPUT_DESCRIPTION_MUTABLE_SINCE_VERSION) {
			zxdg_output_v1_send_description(resource, output->description);
		}
	}
}

static void add_output(struct nx_xdg_output_manager_v1 *manager,
		struct nx_output_layout_output *layout_output) {
	struct nx_xdg_output_v1 *output = calloc(1, sizeof(*output));
	if (output == NULL) {
		return;
	}
	wl_list_init(&output->resources);
	output->layout_output = layout_output;
	output->destroy.notify = handle_output_destroy;
	wl_signal_add(&layout_output->events.destroy, &output->destroy);
	output->description.notify = handle_output_description;
	wl_signal_add(&layout_output->output->events.description,
		&output->description);
	wl_list_insert(&manager->outputs, &output->link);
	output_update(output);
}

static void handle_layout_add(struct wl_listener *listener, void *data) {
	struct nx_xdg_output_manager_v1 *manager =
		wl_container_of(listener, manager, layout_add);
	add_output(manager, data);
}

static void handle_layout_change(struct wl_listener *listener, void *data) {
	struct nx_xdg_output_manager_v1 *manager =
		wl_container_of(listener, manager, layout_change);
	struct nx_xdg_output_v1 *output;
	wl_list_for_each(output, &manager->outputs, link) {
		output_update(output);
	}
}

// The display destroys the global.
static void manager_destroy(struct nx_xdg_output_manager_v1 *manager) {
	struct nx_xdg_output_v1 *output, *tmp;
	wl_list_for_each_safe(output, tmp, &manager->outputs, link) {
		output_destroy(output);
	}
	wl_list_remove(&manager->display_destroy.link);
	wl_list_remove(&manager->layout_add.link);
	wl_list_remove(&manager->layout_change.link);
	wl_list_remove(&manager->layout_destroy.link);
	free(manager);
}

static void handle_layout_destroy(struct wl_listener *listener, void *data) {
	struct nx_xdg_output_manager_v1 *manager =
		wl_container_of(listener, manager, layout_destroy);
	manager_destroy(manager);
}

static void handle_display_destroy(struct wl_listener *listener, void *data) {
	struct nx_xdg_output_manager_v1 *manager =
		wl_container_of(listener, manager, display_destroy);
	manager_destroy(manager);
}

struct nx_xdg_output_manager_v1 *nx_xdg_output_manager_v1_create(
		struct wl_display *display, struct nx_output_layout *layout) {
	struct nx_xdg_output_manager_v1 *manager = calloc(1, sizeof(*manager));
	if (manager == NULL) {
		return NULL;
	}
	manager->layout = layout;
	if (wl_global_create(display, &zxdg_output_manager_v1_interface,
			OUTPUT_MANAGER_VERSION, manager, output_manager_bind) == NULL) {
		free(manager);
		return NULL;
	}

	wl_list_init(&manager->outputs);
	struct nx_output_layout_output *layout_output;
	wl_list_for_each(layout_output, &layout->outputs, link) {
		add_output(manager, layout_output);
	}

	manager->layout_add.notify = handle_layout_add;
	wl_signal_add(&layout->events.add, &manager->layout_add);
	manager->layout_change.notify = handle_layout_change;
	wl_signal_add(&layout->events.change, &manager->layout_change);
	manager->layout_destroy.notify = handle_layout_destroy;
	wl_signal_add(&layout->events.destroy, &manager->layout_destroy);

	manager->display_destroy.notify = handle_display_destroy;
	wl_display_add_destroy_listener(display, &manager->display_destroy);
	return manager;
}
