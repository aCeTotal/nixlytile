#include <assert.h>
#include <stdlib.h>
#include "platform/log/log.h"
#include "platform/output/management/configuration.h"
#include "platform/output/management/head.h"
#include "wlr-output-management-unstable-v1-protocol.h"

static const struct zwlr_output_configuration_v1_interface config_impl;

// NULL once destroyed.
static struct nx_output_configuration_v1 *config_from_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource,
		&zwlr_output_configuration_v1_interface, &config_impl));
	return wl_resource_get_user_data(resource);
}

static struct nx_output_configuration_v1 *config_from_unused_resource(
		struct wl_resource *config_resource) {
	struct nx_output_configuration_v1 *config = config_from_resource(config_resource);
	if (config == NULL || config->finalized) {
		wl_resource_post_error(config_resource,
			ZWLR_OUTPUT_CONFIGURATION_V1_ERROR_ALREADY_USED,
			"configuration object has already been used");
		return NULL;
	}
	return config;
}

static bool config_check_head_is_unconfigured(
		struct nx_output_configuration_v1 *config, struct nx_output *output) {
	struct nx_output_configuration_head_v1 *head;
	wl_list_for_each(head, &config->heads, link) {
		if (head->state.output == output) {
			wl_resource_post_error(config->resource,
				ZWLR_OUTPUT_CONFIGURATION_V1_ERROR_ALREADY_CONFIGURED_HEAD,
				"head has already been configured");
			return false;
		}
	}
	return true;
}

static void config_handle_enable_head(struct wl_client *client,
		struct wl_resource *config_resource, uint32_t id,
		struct wl_resource *head_resource) {
	struct nx_output_configuration_v1 *config =
		config_from_unused_resource(config_resource);
	if (config == NULL) {
		return;
	}
	struct nx_output_head_v1 *head = output_head_from_resource(head_resource);

	// Inert heads get inert resources.
	struct nx_output_configuration_head_v1 *config_head = NULL;
	if (head != NULL) {
		if (!config_check_head_is_unconfigured(config, head->state.output)) {
			return;
		}
		config_head = output_config_head_create(config, head->state.output);
		if (config_head == NULL) {
			wl_resource_post_no_memory(config_resource);
			return;
		}
		config_head->state = head->state;
	}

	output_config_head_create_resource(config_head, config_resource, id);
}

static void config_handle_disable_head(struct wl_client *client,
		struct wl_resource *config_resource,
		struct wl_resource *head_resource) {
	struct nx_output_configuration_v1 *config =
		config_from_unused_resource(config_resource);
	if (config == NULL) {
		return;
	}
	struct nx_output_head_v1 *head = output_head_from_resource(head_resource);
	if (head == NULL) {
		return;
	}
	if (!config_check_head_is_unconfigured(config, head->state.output)) {
		return;
	}

	struct nx_output_configuration_head_v1 *config_head =
		output_config_head_create(config, head->state.output);
	if (config_head == NULL) {
		wl_resource_post_no_memory(config_resource);
		return;
	}
	config_head->state.enabled = false;
}

// Locks the config against reuse.
static void config_finalize(struct nx_output_configuration_v1 *config) {
	if (config->finalized) {
		return;
	}

	// Disabled heads have no resource.
	struct nx_output_configuration_head_v1 *config_head, *tmp;
	wl_list_for_each_safe(config_head, tmp, &config->heads, link) {
		if (config_head->resource == NULL) {
			continue;
		}
		wl_resource_set_user_data(config_head->resource, NULL);
		wl_resource_destroy(config_head->resource);
		config_head->resource = NULL;
	}

	config->finalized = true;
}

// Stale serials cancel the config.
static bool config_validate_serial(struct nx_output_configuration_v1 *config) {
	if (config->serial == config->manager->serial) {
		return true;
	}
	nx_log(NX_DEBUG, "Ignored configuration request: invalid serial");
	zwlr_output_configuration_v1_send_cancelled(config->resource);
	nx_output_configuration_v1_destroy(config);
	return false;
}

static struct nx_output_configuration_v1 *config_submit(
		struct wl_resource *config_resource) {
	struct nx_output_configuration_v1 *config =
		config_from_unused_resource(config_resource);
	if (config == NULL) {
		return NULL;
	}
	config_finalize(config);
	return config_validate_serial(config) ? config : NULL;
}

static void config_handle_apply(struct wl_client *client,
		struct wl_resource *config_resource) {
	struct nx_output_configuration_v1 *config = config_submit(config_resource);
	if (config != NULL) {
		wl_signal_emit_mutable(&config->manager->events.apply, config);
	}
}

static void config_handle_test(struct wl_client *client,
		struct wl_resource *config_resource) {
	struct nx_output_configuration_v1 *config = config_submit(config_resource);
	if (config != NULL) {
		wl_signal_emit_mutable(&config->manager->events.test, config);
	}
}

static void config_handle_destroy(struct wl_client *client,
		struct wl_resource *config_resource) {
	wl_resource_destroy(config_resource);
}

static const struct zwlr_output_configuration_v1_interface config_impl = {
	.enable_head = config_handle_enable_head,
	.disable_head = config_handle_disable_head,
	.apply = config_handle_apply,
	.test = config_handle_test,
	.destroy = config_handle_destroy,
};

static struct nx_output_configuration_v1 *config_create(void) {
	struct nx_output_configuration_v1 *config = calloc(1, sizeof(*config));
	if (config == NULL) {
		return NULL;
	}
	wl_list_init(&config->heads);
	return config;
}

struct nx_output_configuration_v1 *nx_output_configuration_v1_create(void) {
	struct nx_output_configuration_v1 *config = config_create();
	if (config != NULL) {
		config->finalized = true;
	}
	return config;
}

void nx_output_configuration_v1_destroy(
		struct nx_output_configuration_v1 *config) {
	if (config == NULL) {
		return;
	}
	config_finalize(config);
	if (config->resource != NULL) {
		wl_resource_set_user_data(config->resource, NULL);
	}
	struct nx_output_configuration_head_v1 *config_head, *tmp;
	wl_list_for_each_safe(config_head, tmp, &config->heads, link) {
		output_config_head_destroy(config_head);
	}
	free(config);
}

static void config_handle_resource_destroy(struct wl_resource *resource) {
	struct nx_output_configuration_v1 *config = config_from_resource(resource);
	if (config == NULL) {
		return;
	}
	// The compositor owns finalized configs.
	if (config->finalized) {
		config->resource = NULL;
		return;
	}
	nx_output_configuration_v1_destroy(config);
}

struct nx_output_configuration_v1 *output_config_create_for_client(
		struct wl_resource *manager_resource, uint32_t id) {
	struct nx_output_configuration_v1 *config = config_create();
	if (config == NULL) {
		wl_resource_post_no_memory(manager_resource);
		return NULL;
	}

	struct wl_client *client = wl_resource_get_client(manager_resource);
	uint32_t version = wl_resource_get_version(manager_resource);
	config->resource = wl_resource_create(client,
		&zwlr_output_configuration_v1_interface, version, id);
	if (config->resource == NULL) {
		free(config);
		wl_client_post_no_memory(client);
		return NULL;
	}
	wl_resource_set_implementation(config->resource, &config_impl,
		config, config_handle_resource_destroy);
	return config;
}

void nx_output_configuration_v1_send_succeeded(
		struct nx_output_configuration_v1 *config) {
	assert(!config->finished);
	// Client may have destroyed it.
	if (config->resource == NULL) {
		return;
	}
	zwlr_output_configuration_v1_send_succeeded(config->resource);
	config->finished = true;
}

void nx_output_configuration_v1_send_failed(
		struct nx_output_configuration_v1 *config) {
	assert(!config->finished);
	if (config->resource == NULL) {
		return;
	}
	zwlr_output_configuration_v1_send_failed(config->resource);
	config->finished = true;
}
