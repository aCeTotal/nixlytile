#include <assert.h>
#include <stdlib.h>
#include "platform/input/pointer/virtual_pointer_device.h"
#include "wlr-virtual-pointer-unstable-v1-protocol.h"

#define VIRTUAL_POINTER_MANAGER_VERSION 2

static const struct zwlr_virtual_pointer_manager_v1_interface manager_impl;

static struct nx_virtual_pointer_manager_v1 *manager_from_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource,
		&zwlr_virtual_pointer_manager_v1_interface, &manager_impl));
	return wl_resource_get_user_data(resource);
}

static void virtual_pointer_manager_create_virtual_pointer_with_output(
		struct wl_client *client, struct wl_resource *resource,
		struct wl_resource *seat, struct wl_resource *output,
		uint32_t id) {
	struct nx_virtual_pointer_manager_v1 *manager = manager_from_resource(resource);
	struct nx_virtual_pointer_v1 *virtual_pointer =
		input_virtual_pointer_create(resource, id);
	if (virtual_pointer == NULL) {
		return;
	}

	struct nx_virtual_pointer_v1_new_pointer_event event = {
		.new_pointer = virtual_pointer,
		.suggested_output = output ? nx_output_from_resource(output) : NULL,
	};

	wl_list_insert(&manager->virtual_pointers, &virtual_pointer->link);
	wl_signal_emit_mutable(&manager->events.new_virtual_pointer, &event);
}

static void virtual_pointer_manager_create_virtual_pointer(
		struct wl_client *client, struct wl_resource *resource,
		struct wl_resource *seat, uint32_t id) {
	virtual_pointer_manager_create_virtual_pointer_with_output(client,
		resource, seat, NULL, id);
}

static void virtual_pointer_manager_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct zwlr_virtual_pointer_manager_v1_interface manager_impl = {
	.create_virtual_pointer = virtual_pointer_manager_create_virtual_pointer,
	.create_virtual_pointer_with_output = virtual_pointer_manager_create_virtual_pointer_with_output,
	.destroy = virtual_pointer_manager_destroy,
};

static void virtual_pointer_manager_bind(struct wl_client *client, void *data,
		uint32_t version, uint32_t id) {
	struct nx_virtual_pointer_manager_v1 *manager = data;

	struct wl_resource *resource = wl_resource_create(client,
		&zwlr_virtual_pointer_manager_v1_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}

	wl_resource_set_implementation(resource, &manager_impl, manager, NULL);
}

static void handle_display_destroy(struct wl_listener *listener, void *data) {
	struct nx_virtual_pointer_manager_v1 *manager =
		wl_container_of(listener, manager, display_destroy);

	assert(wl_list_empty(&manager->events.new_virtual_pointer.listener_list));

	wl_list_remove(&manager->display_destroy.link);
	wl_global_destroy(manager->global);
	struct nx_virtual_pointer_v1 *pointer, *next;
	wl_list_for_each_safe(pointer, next,
			&manager->virtual_pointers, link) {
		wl_resource_destroy(pointer->resource);
	}
	free(manager);
}

struct nx_virtual_pointer_manager_v1 *nx_virtual_pointer_manager_v1_create(
		struct wl_display *display) {
	struct nx_virtual_pointer_manager_v1 *manager = calloc(1, sizeof(*manager));
	if (!manager) {
		return NULL;
	}

	wl_list_init(&manager->virtual_pointers);

	wl_signal_init(&manager->events.new_virtual_pointer);

	manager->global = wl_global_create(display,
		&zwlr_virtual_pointer_manager_v1_interface,
		VIRTUAL_POINTER_MANAGER_VERSION, manager, virtual_pointer_manager_bind);
	if (!manager->global) {
		free(manager);
		return NULL;
	}

	manager->display_destroy.notify = handle_display_destroy;
	wl_display_add_destroy_listener(display, &manager->display_destroy);
	return manager;
}
