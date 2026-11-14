#include <assert.h>
#include <stdlib.h>
#include "platform/output/management/configuration.h"
#include "platform/output/management/head.h"
#include "wlr-output-management-unstable-v1-protocol.h"

#define OUTPUT_MANAGER_VERSION 4

static const struct zwlr_output_manager_v1_interface manager_impl;

static struct nx_output_manager_v1 *manager_from_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource,
		&zwlr_output_manager_v1_interface, &manager_impl));
	return wl_resource_get_user_data(resource);
}

static void manager_handle_create_configuration(struct wl_client *client,
		struct wl_resource *manager_resource, uint32_t id, uint32_t serial) {
	struct nx_output_manager_v1 *manager =
		manager_from_resource(manager_resource);
	struct nx_output_configuration_v1 *config =
		output_config_create_for_client(manager_resource, id);
	if (config == NULL) {
		return;
	}
	config->manager = manager;
	config->serial = serial;
}

static void manager_handle_stop(struct wl_client *client,
		struct wl_resource *manager_resource) {
	zwlr_output_manager_v1_send_finished(manager_resource);
	wl_resource_destroy(manager_resource);
}

static const struct zwlr_output_manager_v1_interface manager_impl = {
	.create_configuration = manager_handle_create_configuration,
	.stop = manager_handle_stop,
};

static void manager_handle_resource_destroy(struct wl_resource *resource) {
	wl_list_remove(wl_resource_get_link(resource));
}

static void manager_bind(struct wl_client *client, void *data, uint32_t version,
		uint32_t id) {
	struct nx_output_manager_v1 *manager = data;

	struct wl_resource *resource = wl_resource_create(client,
		&zwlr_output_manager_v1_interface, version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &manager_impl, manager,
		manager_handle_resource_destroy);

	wl_list_insert(&manager->resources, wl_resource_get_link(resource));

	struct nx_output_head_v1 *head;
	wl_list_for_each(head, &manager->heads, link) {
		output_manager_send_head(head, resource);
	}

	zwlr_output_manager_v1_send_done(resource, manager->serial);
}

static void manager_handle_display_destroy(struct wl_listener *listener,
		void *data) {
	struct nx_output_manager_v1 *manager =
		wl_container_of(listener, manager, display_destroy);

	assert(wl_list_empty(&manager->events.apply.listener_list));
	assert(wl_list_empty(&manager->events.test.listener_list));

	wl_list_remove(&manager->display_destroy.link);
	struct nx_output_head_v1 *head, *tmp;
	wl_list_for_each_safe(head, tmp, &manager->heads, link) {
		output_head_destroy(head);
	}
	wl_global_destroy(manager->global);
	free(manager);
}

struct nx_output_manager_v1 *nx_output_manager_v1_create(
		struct wl_display *display) {
	struct nx_output_manager_v1 *manager = calloc(1, sizeof(*manager));
	if (manager == NULL) {
		return NULL;
	}
	manager->display = display;

	wl_list_init(&manager->resources);
	wl_list_init(&manager->heads);

	wl_signal_init(&manager->events.apply);
	wl_signal_init(&manager->events.test);

	manager->global = wl_global_create(display,
		&zwlr_output_manager_v1_interface, OUTPUT_MANAGER_VERSION,
		manager, manager_bind);
	if (manager->global == NULL) {
		free(manager);
		return NULL;
	}

	manager->display_destroy.notify = manager_handle_display_destroy;
	wl_display_add_destroy_listener(display, &manager->display_destroy);
	return manager;
}
