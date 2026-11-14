#include <assert.h>
#include <stdlib.h>
#include "platform/output/management/head.h"
#include "wlr-output-management-unstable-v1-protocol.h"

static const struct zwlr_output_head_v1_interface head_impl;

struct nx_output_head_v1 *output_head_from_resource(struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource,
		&zwlr_output_head_v1_interface, &head_impl));
	return wl_resource_get_user_data(resource);
}

static void make_inert(struct wl_resource *resource) {
	wl_list_remove(wl_resource_get_link(resource));
	wl_list_init(wl_resource_get_link(resource));
	wl_resource_set_user_data(resource, NULL);
}

void output_head_destroy(struct nx_output_head_v1 *head) {
	struct wl_resource *resource, *tmp;
	wl_resource_for_each_safe(resource, tmp, &head->mode_resources) {
		zwlr_output_mode_v1_send_finished(resource);
		make_inert(resource);
	}
	wl_resource_for_each_safe(resource, tmp, &head->resources) {
		zwlr_output_head_v1_send_finished(resource);
		make_inert(resource);
	}
	wl_list_remove(&head->link);
	wl_list_remove(&head->output_destroy.link);
	free(head);
}

static void head_handle_output_destroy(struct wl_listener *listener,
		void *data) {
	struct nx_output_head_v1 *head =
		wl_container_of(listener, head, output_destroy);
	head->manager->current_configuration_dirty = true;
	output_head_destroy(head);
}

struct nx_output_head_v1 *output_head_create(struct nx_output_manager_v1 *manager,
		struct nx_output *output) {
	struct nx_output_head_v1 *head = calloc(1, sizeof(*head));
	if (head == NULL) {
		return NULL;
	}
	head->manager = manager;
	head->state.output = output;
	wl_list_init(&head->resources);
	wl_list_init(&head->mode_resources);
	wl_list_insert(&manager->heads, &head->link);
	head->output_destroy.notify = head_handle_output_destroy;
	wl_signal_add(&output->events.destroy, &head->output_destroy);
	return head;
}

static void head_handle_resource_destroy(struct wl_resource *resource) {
	wl_list_remove(wl_resource_get_link(resource));
}

static void head_handle_release(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct zwlr_output_head_v1_interface head_impl = {
	.release = head_handle_release,
};

static void send_identity(struct nx_output *output,
		struct wl_resource *head_resource) {
	uint32_t version = wl_resource_get_version(head_resource);
	zwlr_output_head_v1_send_name(head_resource, output->name);
	zwlr_output_head_v1_send_description(head_resource,
		output->description ? output->description : "Unknown");

	if (output->phys_width > 0 && output->phys_height > 0) {
		zwlr_output_head_v1_send_physical_size(head_resource,
			output->phys_width, output->phys_height);
	}
	if (version >= ZWLR_OUTPUT_HEAD_V1_MAKE_SINCE_VERSION && output->make != NULL) {
		zwlr_output_head_v1_send_make(head_resource, output->make);
	}
	if (version >= ZWLR_OUTPUT_HEAD_V1_MODEL_SINCE_VERSION && output->model != NULL) {
		zwlr_output_head_v1_send_model(head_resource, output->model);
	}
	if (version >= ZWLR_OUTPUT_HEAD_V1_SERIAL_NUMBER_SINCE_VERSION &&
			output->serial != NULL) {
		zwlr_output_head_v1_send_serial_number(head_resource, output->serial);
	}
}

void output_manager_send_head(struct nx_output_head_v1 *head,
		struct wl_resource *manager_resource) {
	struct nx_output *output = head->state.output;

	struct wl_client *client = wl_resource_get_client(manager_resource);
	uint32_t version = wl_resource_get_version(manager_resource);
	struct wl_resource *head_resource = wl_resource_create(client,
		&zwlr_output_head_v1_interface, version, 0);
	if (head_resource == NULL) {
		wl_resource_post_no_memory(manager_resource);
		return;
	}
	wl_resource_set_implementation(head_resource, &head_impl, head,
		head_handle_resource_destroy);
	wl_list_insert(&head->resources, wl_resource_get_link(head_resource));

	zwlr_output_manager_v1_send_head(manager_resource, head_resource);
	send_identity(output, head_resource);

	struct nx_output_mode *mode;
	wl_list_for_each(mode, &output->modes, link) {
		output_head_send_mode(head, head_resource, mode);
	}
	// Virtual mode for modeless outputs.
	if (head->state.mode == NULL && head->state.enabled) {
		output_head_send_mode(head, head_resource, NULL);
	}

	output_head_send_state(head, head_resource, HEAD_STATE_ALL);
}
