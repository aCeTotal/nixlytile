#include <assert.h>
#include <stdlib.h>
#include "platform/capture/image_copy.h"
#include "platform/capture/image_copy/session.h"
#include "ext-image-copy-capture-v1-protocol.h"

#define IMAGE_COPY_CAPTURE_MANAGER_V1_VERSION 1

static void manager_handle_create_session(struct wl_client *client,
		struct wl_resource *manager_resource, uint32_t new_id,
		struct wl_resource *source_resource, uint32_t options) {
	image_copy_session_create(manager_resource, new_id,
		capture_source_from_resource(source_resource), options);
}

static void manager_handle_create_pointer_cursor_session(
		struct wl_client *client, struct wl_resource *manager_resource,
		uint32_t new_id, struct wl_resource *source_resource,
		struct wl_resource *pointer_resource) {
	image_copy_cursor_session_create(manager_resource, new_id,
		source_resource, pointer_resource);
}

static void manager_handle_destroy(struct wl_client *client,
		struct wl_resource *manager_resource) {
	wl_resource_destroy(manager_resource);
}

static const struct ext_image_copy_capture_manager_v1_interface manager_impl = {
	.create_session = manager_handle_create_session,
	.create_pointer_cursor_session = manager_handle_create_pointer_cursor_session,
	.destroy = manager_handle_destroy,
};

static void manager_bind(struct wl_client *client, void *data,
		uint32_t version, uint32_t id) {
	struct wl_resource *resource = wl_resource_create(client,
		&ext_image_copy_capture_manager_v1_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &manager_impl, data, NULL);
}

static void manager_handle_display_destroy(struct wl_listener *listener,
		void *data) {
	struct nx_ext_image_copy_capture_manager_v1 *manager =
		wl_container_of(listener, manager, display_destroy);
	wl_list_remove(&manager->display_destroy.link);
	wl_global_destroy(manager->global);
	free(manager);
}

struct nx_ext_image_copy_capture_manager_v1 *nx_ext_image_copy_capture_manager_v1_create(
		struct wl_display *display, uint32_t version) {
	assert(version <= IMAGE_COPY_CAPTURE_MANAGER_V1_VERSION);

	struct nx_ext_image_copy_capture_manager_v1 *manager =
		calloc(1, sizeof(*manager));
	if (manager == NULL) {
		return NULL;
	}

	manager->global = wl_global_create(display,
		&ext_image_copy_capture_manager_v1_interface, version, manager,
		manager_bind);
	if (manager->global == NULL) {
		free(manager);
		return NULL;
	}

	manager->display_destroy.notify = manager_handle_display_destroy;
	wl_display_add_destroy_listener(display, &manager->display_destroy);

	return manager;
}
