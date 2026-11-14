#include <assert.h>
#include <stdlib.h>
#include "platform/clipboard/control/data_control.h"
#include "platform/clipboard/control/data_control_v1.h"
#include "platform/clipboard/control/ext_data_control.h"
#include "ext-data-control-v1-protocol.h"
#include "wlr-data-control-unstable-v1-protocol.h"

#define ZWLR_MANAGER_VERSION 2
#define EXT_MANAGER_VERSION 1

_Static_assert(
	ZWLR_DATA_CONTROL_DEVICE_V1_DATA_OFFER == EXT_DATA_CONTROL_DEVICE_V1_DATA_OFFER &&
	ZWLR_DATA_CONTROL_DEVICE_V1_SELECTION == EXT_DATA_CONTROL_DEVICE_V1_SELECTION &&
	ZWLR_DATA_CONTROL_DEVICE_V1_FINISHED == EXT_DATA_CONTROL_DEVICE_V1_FINISHED &&
	ZWLR_DATA_CONTROL_DEVICE_V1_PRIMARY_SELECTION ==
		EXT_DATA_CONTROL_DEVICE_V1_PRIMARY_SELECTION &&
	(int)ZWLR_DATA_CONTROL_DEVICE_V1_ERROR_USED_SOURCE ==
		(int)EXT_DATA_CONTROL_DEVICE_V1_ERROR_USED_SOURCE &&
	ZWLR_DATA_CONTROL_SOURCE_V1_SEND == EXT_DATA_CONTROL_SOURCE_V1_SEND &&
	ZWLR_DATA_CONTROL_SOURCE_V1_CANCELLED == EXT_DATA_CONTROL_SOURCE_V1_CANCELLED &&
	(int)ZWLR_DATA_CONTROL_SOURCE_V1_ERROR_INVALID_OFFER ==
		(int)EXT_DATA_CONTROL_SOURCE_V1_ERROR_INVALID_OFFER &&
	ZWLR_DATA_CONTROL_OFFER_V1_OFFER == EXT_DATA_CONTROL_OFFER_V1_OFFER,
	"zwlr and ext data-control must share opcodes");

static const struct control_protocol zwlr_protocol = {
	.manager = &zwlr_data_control_manager_v1_interface,
	.device = &zwlr_data_control_device_v1_interface,
	.source = &zwlr_data_control_source_v1_interface,
	.offer = &zwlr_data_control_offer_v1_interface,
	.primary_selection_since =
		ZWLR_DATA_CONTROL_DEVICE_V1_PRIMARY_SELECTION_SINCE_VERSION,
};

static const struct control_protocol ext_protocol = {
	.manager = &ext_data_control_manager_v1_interface,
	.device = &ext_data_control_device_v1_interface,
	.source = &ext_data_control_source_v1_interface,
	.offer = &ext_data_control_offer_v1_interface,
	.primary_selection_since =
		EXT_DATA_CONTROL_DEVICE_V1_PRIMARY_SELECTION_SINCE_VERSION,
};

static const struct ext_data_control_manager_v1_interface manager_impl;

static const struct control_protocol *protocol_from_manager_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource,
			&ext_data_control_manager_v1_interface, &manager_impl) ||
		wl_resource_instance_of(resource,
			&zwlr_data_control_manager_v1_interface, &manager_impl));
	return wl_resource_get_user_data(resource);
}

static void manager_handle_create_data_source(struct wl_client *client,
		struct wl_resource *manager_resource, uint32_t id) {
	control_source_create(protocol_from_manager_resource(manager_resource),
		manager_resource, id);
}

static void manager_handle_get_data_device(struct wl_client *client,
		struct wl_resource *manager_resource, uint32_t id,
		struct wl_resource *seat_resource) {
	control_device_create(protocol_from_manager_resource(manager_resource),
		manager_resource, id, seat_resource);
}

static void manager_handle_destroy(struct wl_client *client,
		struct wl_resource *manager_resource) {
	wl_resource_destroy(manager_resource);
}

static const struct ext_data_control_manager_v1_interface manager_impl = {
	.create_data_source = manager_handle_create_data_source,
	.get_data_device = manager_handle_get_data_device,
	.destroy = manager_handle_destroy,
};

static void manager_bind(struct wl_client *client, void *data, uint32_t version,
		uint32_t id) {
	const struct control_protocol *protocol = data;

	struct wl_resource *resource = wl_resource_create(client,
		protocol->manager, version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &manager_impl, data, NULL);
}

static struct wl_global *control_global_create(struct wl_display *display,
		const struct control_protocol *protocol, uint32_t version) {
	return wl_global_create(display, protocol->manager, version,
		(void *)protocol, manager_bind);
}

static void zwlr_handle_display_destroy(struct wl_listener *listener,
		void *data) {
	struct nx_data_control_manager_v1 *manager =
		wl_container_of(listener, manager, display_destroy);
	wl_list_remove(&manager->display_destroy.link);
	wl_global_destroy(manager->global);
	free(manager);
}

struct nx_data_control_manager_v1 *nx_data_control_manager_v1_create(
		struct wl_display *display) {
	struct nx_data_control_manager_v1 *manager = calloc(1, sizeof(*manager));
	if (manager == NULL) {
		return NULL;
	}

	manager->global = control_global_create(display, &zwlr_protocol,
		ZWLR_MANAGER_VERSION);
	if (manager->global == NULL) {
		free(manager);
		return NULL;
	}

	manager->display_destroy.notify = zwlr_handle_display_destroy;
	wl_display_add_destroy_listener(display, &manager->display_destroy);

	return manager;
}

static void ext_handle_display_destroy(struct wl_listener *listener,
		void *data) {
	struct nx_ext_data_control_manager_v1 *manager =
		wl_container_of(listener, manager, display_destroy);
	wl_list_remove(&manager->display_destroy.link);
	wl_global_destroy(manager->global);
	free(manager);
}

struct nx_ext_data_control_manager_v1 *nx_ext_data_control_manager_v1_create(
		struct wl_display *display, uint32_t version) {
	assert(version <= EXT_MANAGER_VERSION);

	struct nx_ext_data_control_manager_v1 *manager = calloc(1, sizeof(*manager));
	if (manager == NULL) {
		return NULL;
	}

	manager->global = control_global_create(display, &ext_protocol, version);
	if (manager->global == NULL) {
		free(manager);
		return NULL;
	}

	manager->display_destroy.notify = ext_handle_display_destroy;
	wl_display_add_destroy_listener(display, &manager->display_destroy);

	return manager;
}
