#include <assert.h>
#include <stdlib.h>
#include "color-management-v1-protocol.h"
#include "platform/output/output.h"
#include "platform/surface/color/image_description.h"
#include "platform/surface/color/manager_requests.h"

struct color_output {
	struct wl_resource *resource;
	struct nx_output *output;
	struct nx_color_manager_v1 *manager;

	struct wl_listener output_destroy;
};

static const struct wp_color_management_output_v1_interface color_output_impl;

static struct color_output *color_output_from_resource(struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &wp_color_management_output_v1_interface,
		&color_output_impl));
	return wl_resource_get_user_data(resource);
}

static void color_output_handle_get_image_description(struct wl_client *client,
		struct wl_resource *color_output_resource, uint32_t id) {
	struct color_output *color_output = color_output_from_resource(color_output_resource);
	if (color_output == NULL) {
		static const struct color_image_desc_failure output_gone = {
			.cause = WP_IMAGE_DESCRIPTION_V1_CAUSE_NO_OUTPUT,
			.msg = "output has been destroyed",
		};
		color_image_desc_create_failed(color_output_resource, id, &output_gone);
		return;
	}

	struct nx_image_description_v1_data data = {
		.tf_named = WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22,
		.primaries_named = WP_COLOR_MANAGER_V1_PRIMARIES_SRGB,
	};
	const struct nx_output_image_description *image_desc =
		color_output->output->image_description;
	if (image_desc != NULL) {
		data.tf_named = nx_color_manager_v1_transfer_function_from_nx(image_desc->transfer_function);
		data.primaries_named = nx_color_manager_v1_primaries_from_nx(image_desc->primaries);
	}

	const struct color_image_desc_ready ready = {
		.manager = color_output->manager,
		.data = &data,
		.get_info_allowed = true,
	};
	color_image_desc_create_ready(color_output_resource, id, &ready);
}

static void color_output_handle_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct wp_color_management_output_v1_interface color_output_impl = {
	.destroy = color_output_handle_destroy,
	.get_image_description = color_output_handle_get_image_description,
};

static void color_output_destroy(struct color_output *color_output) {
	if (color_output == NULL) {
		return;
	}
	wl_resource_set_user_data(color_output->resource, NULL);
	wl_list_remove(&color_output->output_destroy.link);
	free(color_output);
}

static void color_output_handle_output_destroy(struct wl_listener *listener, void *data) {
	struct color_output *color_output =
		wl_container_of(listener, color_output, output_destroy);
	color_output_destroy(color_output);
}

static void color_output_handle_resource_destroy(struct wl_resource *resource) {
	color_output_destroy(color_output_from_resource(resource));
}

void color_manager_get_output(struct wl_client *client,
		struct wl_resource *manager_resource, uint32_t id,
		struct wl_resource *output_resource) {
	struct nx_color_manager_v1 *manager = color_manager_from_resource(manager_resource);
	struct nx_output *output = nx_output_from_resource(output_resource);

	uint32_t version = wl_resource_get_version(manager_resource);
	struct wl_resource *color_output_resource = wl_resource_create(client,
		&wp_color_management_output_v1_interface, version, id);
	if (!color_output_resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(color_output_resource, &color_output_impl,
		NULL, color_output_handle_resource_destroy);

	// Inert without an output
	if (output == NULL) {
		return;
	}

	struct color_output *color_output = calloc(1, sizeof(*color_output));
	if (color_output == NULL) {
		wl_client_post_no_memory(client);
		return;
	}

	color_output->resource = color_output_resource;
	color_output->manager = manager;
	color_output->output = output;

	color_output->output_destroy.notify = color_output_handle_output_destroy;
	wl_signal_add(&output->events.destroy, &color_output->output_destroy);

	wl_resource_set_user_data(color_output->resource, color_output);
}
