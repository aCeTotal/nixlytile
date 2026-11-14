#include <assert.h>
#include <stdlib.h>
#include "platform/output/management/configuration.h"
#include "platform/output/management/head.h"
#include "wlr-output-management-unstable-v1-protocol.h"

static const struct zwlr_output_configuration_head_v1_interface config_head_impl;

void output_config_head_destroy(struct nx_output_configuration_head_v1 *config_head) {
	if (config_head == NULL) {
		return;
	}
	if (config_head->resource != NULL) {
		wl_resource_set_user_data(config_head->resource, NULL);
	}
	wl_list_remove(&config_head->link);
	wl_list_remove(&config_head->output_destroy.link);
	free(config_head);
}

static void config_head_handle_output_destroy(struct wl_listener *listener,
		void *data) {
	struct nx_output_configuration_head_v1 *config_head =
		wl_container_of(listener, config_head, output_destroy);
	output_config_head_destroy(config_head);
}

struct nx_output_configuration_head_v1 *output_config_head_create(
		struct nx_output_configuration_v1 *config, struct nx_output *output) {
	struct nx_output_configuration_head_v1 *config_head =
		calloc(1, sizeof(*config_head));
	if (config_head == NULL) {
		return NULL;
	}
	config_head->state.output = output;
	wl_list_insert(&config->heads, &config_head->link);
	config_head->output_destroy.notify = config_head_handle_output_destroy;
	wl_signal_add(&output->events.destroy, &config_head->output_destroy);
	return config_head;
}

struct nx_output_configuration_head_v1 *nx_output_configuration_head_v1_create(
		struct nx_output_configuration_v1 *config, struct nx_output *output) {
	struct nx_output_configuration_head_v1 *config_head =
		output_config_head_create(config, output);
	if (config_head == NULL) {
		return NULL;
	}
	config_head->state.enabled = output->enabled;
	config_head->state.mode = output->current_mode;
	config_head->state.custom_mode.width = output->width;
	config_head->state.custom_mode.height = output->height;
	config_head->state.custom_mode.refresh = output->refresh;
	config_head->state.transform = output->transform;
	config_head->state.scale = output->scale;
	config_head->state.adaptive_sync_enabled =
		output->adaptive_sync_status == NX_OUTPUT_ADAPTIVE_SYNC_ENABLED;
	return config_head;
}

// NULL once the head is inert.
static struct nx_output_configuration_head_v1 *config_head_from_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource,
		&zwlr_output_configuration_head_v1_interface, &config_head_impl));
	return wl_resource_get_user_data(resource);
}

static bool output_has_mode(struct nx_output *output, struct nx_output_mode *mode) {
	struct nx_output_mode *m;
	wl_list_for_each(m, &output->modes, link) {
		if (m == mode) {
			return true;
		}
	}
	return false;
}

static void config_head_handle_set_mode(struct wl_client *client,
		struct wl_resource *config_head_resource,
		struct wl_resource *mode_resource) {
	struct nx_output_configuration_head_v1 *config_head =
		config_head_from_resource(config_head_resource);
	if (config_head == NULL) {
		return;
	}

	// NULL is the virtual mode.
	struct nx_output_mode *mode = output_mode_from_resource(mode_resource);
	if (mode != NULL && !output_has_mode(config_head->state.output, mode)) {
		wl_resource_post_error(config_head_resource,
			ZWLR_OUTPUT_CONFIGURATION_HEAD_V1_ERROR_INVALID_MODE,
			"mode doesn't belong to head");
		return;
	}

	config_head->state.mode = mode;
	if (mode != NULL) {
		config_head->state.custom_mode.width = 0;
		config_head->state.custom_mode.height = 0;
		config_head->state.custom_mode.refresh = 0;
	}
}

static void config_head_handle_set_custom_mode(struct wl_client *client,
		struct wl_resource *config_head_resource, int32_t width, int32_t height,
		int32_t refresh) {
	struct nx_output_configuration_head_v1 *config_head =
		config_head_from_resource(config_head_resource);
	if (config_head == NULL) {
		return;
	}

	if (width <= 0 || height <= 0 || refresh < 0) {
		wl_resource_post_error(config_head_resource,
			ZWLR_OUTPUT_CONFIGURATION_HEAD_V1_ERROR_INVALID_CUSTOM_MODE,
			"invalid custom mode");
		return;
	}

	config_head->state.mode = NULL;
	config_head->state.custom_mode.width = width;
	config_head->state.custom_mode.height = height;
	config_head->state.custom_mode.refresh = refresh;
}

static void config_head_handle_set_position(struct wl_client *client,
		struct wl_resource *config_head_resource, int32_t x, int32_t y) {
	struct nx_output_configuration_head_v1 *config_head =
		config_head_from_resource(config_head_resource);
	if (config_head == NULL) {
		return;
	}

	config_head->state.x = x;
	config_head->state.y = y;
}

static void config_head_handle_set_transform(struct wl_client *client,
		struct wl_resource *config_head_resource, int32_t transform) {
	struct nx_output_configuration_head_v1 *config_head =
		config_head_from_resource(config_head_resource);
	if (config_head == NULL) {
		return;
	}

	if (!wl_output_transform_is_valid(transform, 1)) {
		wl_resource_post_error(config_head_resource,
			ZWLR_OUTPUT_CONFIGURATION_HEAD_V1_ERROR_INVALID_TRANSFORM,
			"invalid transform");
		return;
	}

	config_head->state.transform = transform;
}

static void config_head_handle_set_scale(struct wl_client *client,
		struct wl_resource *config_head_resource, wl_fixed_t scale_fixed) {
	struct nx_output_configuration_head_v1 *config_head =
		config_head_from_resource(config_head_resource);
	if (config_head == NULL) {
		return;
	}

	float scale = (float)wl_fixed_to_double(scale_fixed);
	if (scale <= 0) {
		wl_resource_post_error(config_head_resource,
			ZWLR_OUTPUT_CONFIGURATION_HEAD_V1_ERROR_INVALID_SCALE,
			"invalid scale");
		return;
	}

	config_head->state.scale = scale;
}

static void config_head_handle_set_adaptive_sync(struct wl_client *client,
		struct wl_resource *config_head_resource, uint32_t state) {
	struct nx_output_configuration_head_v1 *config_head =
		config_head_from_resource(config_head_resource);
	if (config_head == NULL) {
		return;
	}

	switch (state) {
	case ZWLR_OUTPUT_HEAD_V1_ADAPTIVE_SYNC_STATE_ENABLED:
		config_head->state.adaptive_sync_enabled = true;
		break;
	case ZWLR_OUTPUT_HEAD_V1_ADAPTIVE_SYNC_STATE_DISABLED:
		config_head->state.adaptive_sync_enabled = false;
		break;
	default:
		wl_resource_post_error(config_head_resource,
			ZWLR_OUTPUT_CONFIGURATION_HEAD_V1_ERROR_INVALID_ADAPTIVE_SYNC_STATE,
			"client requested invalid adaptive sync state %ul", state);
		break;
	}
}

static const struct zwlr_output_configuration_head_v1_interface config_head_impl = {
	.set_mode = config_head_handle_set_mode,
	.set_custom_mode = config_head_handle_set_custom_mode,
	.set_position = config_head_handle_set_position,
	.set_transform = config_head_handle_set_transform,
	.set_scale = config_head_handle_set_scale,
	.set_adaptive_sync = config_head_handle_set_adaptive_sync,
};

static void config_head_handle_resource_destroy(struct wl_resource *resource) {
	output_config_head_destroy(config_head_from_resource(resource));
}

void output_config_head_create_resource(
		struct nx_output_configuration_head_v1 *config_head,
		struct wl_resource *config_resource, uint32_t id) {
	struct wl_client *client = wl_resource_get_client(config_resource);
	uint32_t version = wl_resource_get_version(config_resource);
	struct wl_resource *resource = wl_resource_create(client,
		&zwlr_output_configuration_head_v1_interface, version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &config_head_impl,
		config_head, config_head_handle_resource_destroy);

	if (config_head != NULL) {
		config_head->resource = resource;
		config_head->state.enabled = true;
	}
}
