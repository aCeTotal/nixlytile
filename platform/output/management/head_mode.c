#include <assert.h>
#include "platform/output/management/head.h"
#include "wlr-output-management-unstable-v1-protocol.h"

static const struct zwlr_output_mode_v1_interface output_mode_impl;

struct nx_output_mode *output_mode_from_resource(struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource,
		&zwlr_output_mode_v1_interface, &output_mode_impl));
	return wl_resource_get_user_data(resource);
}

static void send_mode_state(struct wl_resource *mode_resource,
		struct nx_output_mode *mode) {
	zwlr_output_mode_v1_send_size(mode_resource, mode->width, mode->height);
	if (mode->refresh > 0) {
		zwlr_output_mode_v1_send_refresh(mode_resource, mode->refresh);
	}
	if (mode->preferred) {
		zwlr_output_mode_v1_send_preferred(mode_resource);
	}
}

static void output_mode_handle_resource_destroy(struct wl_resource *resource) {
	wl_list_remove(wl_resource_get_link(resource));
}

static void output_mode_handle_release(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct zwlr_output_mode_v1_interface output_mode_impl = {
	.release = output_mode_handle_release,
};

void output_head_send_mode(struct nx_output_head_v1 *head,
		struct wl_resource *head_resource, struct nx_output_mode *mode) {
	struct wl_client *client = wl_resource_get_client(head_resource);
	uint32_t version = wl_resource_get_version(head_resource);
	struct wl_resource *mode_resource =
		wl_resource_create(client, &zwlr_output_mode_v1_interface, version, 0);
	if (mode_resource == NULL) {
		wl_resource_post_no_memory(head_resource);
		return;
	}
	wl_resource_set_implementation(mode_resource, &output_mode_impl, mode,
		output_mode_handle_resource_destroy);
	wl_list_insert(&head->mode_resources, wl_resource_get_link(mode_resource));

	zwlr_output_head_v1_send_mode(head_resource, mode_resource);
	if (mode != NULL) {
		send_mode_state(mode_resource, mode);
	}
}

static bool head_has_mode_resource(struct nx_output_head_v1 *head,
		struct nx_output_mode *mode) {
	struct wl_resource *resource;
	wl_resource_for_each(resource, &head->mode_resources) {
		if (output_mode_from_resource(resource) == mode) {
			return true;
		}
	}
	return false;
}

static void head_send_mode_to_all(struct nx_output_head_v1 *head,
		struct nx_output_mode *mode) {
	struct wl_resource *resource;
	wl_resource_for_each(resource, &head->resources) {
		output_head_send_mode(head, resource, mode);
	}
}

static void head_destroy_custom_mode_resources(struct nx_output_head_v1 *head) {
	struct wl_resource *resource, *tmp;
	wl_resource_for_each_safe(resource, tmp, &head->mode_resources) {
		if (wl_resource_get_user_data(resource) != NULL) {
			continue;
		}
		zwlr_output_mode_v1_send_finished(resource);
		wl_list_remove(wl_resource_get_link(resource));
		wl_list_init(wl_resource_get_link(resource));
	}
}

void output_head_update_modes(struct nx_output_head_v1 *head,
		const struct nx_output_head_v1_state *next) {
	struct nx_output_mode *mode;
	wl_list_for_each(mode, &head->state.output->modes, link) {
		if (!head_has_mode_resource(head, mode)) {
			head_send_mode_to_all(head, mode);
		}
	}

	if (next->mode != NULL) {
		head_destroy_custom_mode_resources(head);
		return;
	}
	// NULL is the virtual mode.
	if (next->enabled && !head_has_mode_resource(head, NULL)) {
		head_send_mode_to_all(head, NULL);
	}
}

static struct wl_resource *find_mode_resource(struct nx_output_head_v1 *head,
		struct wl_client *client) {
	struct wl_resource *mode_resource;
	wl_resource_for_each(mode_resource, &head->mode_resources) {
		if (wl_resource_get_client(mode_resource) == client &&
				output_mode_from_resource(mode_resource) == head->state.mode) {
			return mode_resource;
		}
	}
	return NULL;
}

void output_head_send_current_mode(struct nx_output_head_v1 *head,
		struct wl_resource *head_resource) {
	struct wl_resource *mode_resource =
		find_mode_resource(head, wl_resource_get_client(head_resource));
	assert(mode_resource != NULL);

	// Virtual mode for modeless outputs.
	if (head->state.mode == NULL) {
		struct nx_output_mode virtual_mode = {
			.width = head->state.custom_mode.width,
			.height = head->state.custom_mode.height,
			.refresh = head->state.custom_mode.refresh,
		};
		send_mode_state(mode_resource, &virtual_mode);
	}

	zwlr_output_head_v1_send_current_mode(head_resource, mode_resource);
}
