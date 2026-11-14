#include <assert.h>
#include <stdlib.h>
#include "color-management-v1-protocol.h"
#include "platform/container/mem.h"
#include "platform/surface/color/manager_requests.h"

// Pinned: v2 events stay unimplemented
#define COLOR_MANAGEMENT_V1_VERSION 1

static const struct wp_color_manager_v1_interface manager_impl;

struct nx_color_manager_v1 *color_manager_from_resource(struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &wp_color_manager_v1_interface, &manager_impl));
	return wl_resource_get_user_data(resource);
}

static void manager_handle_destroy(struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static void manager_handle_create_windows_scrgb(struct wl_client *client,
		struct wl_resource *manager_resource, uint32_t id) {
	wl_resource_post_error(manager_resource,
		WP_COLOR_MANAGER_V1_ERROR_UNSUPPORTED_FEATURE,
		"get_windows_scrgb is not supported");
}

static const struct wp_color_manager_v1_interface manager_impl = {
	.destroy = manager_handle_destroy,
	.get_output = color_manager_get_output,
	.get_surface = color_manager_get_surface,
	.get_surface_feedback = color_manager_get_surface_feedback,
	.create_icc_creator = color_manager_create_icc_creator,
	.create_parametric_creator = color_manager_create_parametric_creator,
	.create_windows_scrgb = manager_handle_create_windows_scrgb,
};

static void send_supported_features(struct wl_resource *resource,
		const struct nx_color_manager_v1_features *features) {
	const bool supported[] = {
		[WP_COLOR_MANAGER_V1_FEATURE_ICC_V2_V4] = features->icc_v2_v4,
		[WP_COLOR_MANAGER_V1_FEATURE_PARAMETRIC] = features->parametric,
		[WP_COLOR_MANAGER_V1_FEATURE_SET_PRIMARIES] = features->set_primaries,
		[WP_COLOR_MANAGER_V1_FEATURE_SET_LUMINANCES] = features->set_luminances,
		[WP_COLOR_MANAGER_V1_FEATURE_SET_MASTERING_DISPLAY_PRIMARIES] =
			features->set_mastering_display_primaries,
	};

	for (uint32_t i = 0; i < sizeof(supported) / sizeof(supported[0]); i++) {
		if (supported[i]) {
			wp_color_manager_v1_send_supported_feature(resource, i);
		}
	}
}

static void manager_bind(struct wl_client *client, void *data,
		uint32_t version, uint32_t id) {
	struct nx_color_manager_v1 *manager = data;

	struct wl_resource *resource = wl_resource_create(client,
		&wp_color_manager_v1_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &manager_impl, manager, NULL);

	send_supported_features(resource, &manager->features);
	for (size_t i = 0; i < manager->render_intents_len; i++) {
		wp_color_manager_v1_send_supported_intent(resource,
			manager->render_intents[i]);
	}
	for (size_t i = 0; i < manager->transfer_functions_len; i++) {
		enum wp_color_manager_v1_transfer_function tf = manager->transfer_functions[i];
		if (wp_color_manager_v1_transfer_function_is_valid(tf, version)) {
			wp_color_manager_v1_send_supported_tf_named(resource, tf);
		}
	}
	for (size_t i = 0; i < manager->primaries_len; i++) {
		wp_color_manager_v1_send_supported_primaries_named(resource,
			manager->primaries[i]);
	}

	wp_color_manager_v1_send_done(resource);
}

static void manager_free(struct nx_color_manager_v1 *manager) {
	free(manager->render_intents);
	free(manager->transfer_functions);
	free(manager->primaries);
	free(manager);
}

static void manager_handle_display_destroy(struct wl_listener *listener, void *data) {
	struct nx_color_manager_v1 *manager = wl_container_of(listener, manager, display_destroy);
	wl_signal_emit_mutable(&manager->events.destroy, NULL);
	assert(wl_list_empty(&manager->events.destroy.listener_list));
	wl_list_remove(&manager->display_destroy.link);
	wl_global_destroy(manager->global);
	manager_free(manager);
}

static void assert_valid_options(const struct nx_color_manager_v1_options *options) {
	bool has_perceptual_render_intent = false;
	for (size_t i = 0; i < options->render_intents_len; i++) {
		if (options->render_intents[i] == WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL) {
			has_perceptual_render_intent = true;
		}
	}
	assert(has_perceptual_render_intent);

	for (size_t i = 0; i < options->transfer_functions_len; i++) {
		assert(options->transfer_functions[i] != WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_SRGB);
	}
}

static bool copy_options(struct nx_color_manager_v1 *manager,
		const struct nx_color_manager_v1_options *options) {
	manager->features = options->features;
	manager->render_intents_len = options->render_intents_len;
	manager->transfer_functions_len = options->transfer_functions_len;
	manager->primaries_len = options->primaries_len;
	return memdup(&manager->render_intents, options->render_intents,
			sizeof(options->render_intents[0]) * options->render_intents_len) &&
		memdup(&manager->transfer_functions, options->transfer_functions,
			sizeof(options->transfer_functions[0]) * options->transfer_functions_len) &&
		memdup(&manager->primaries, options->primaries,
			sizeof(options->primaries[0]) * options->primaries_len);
}

struct nx_color_manager_v1 *nx_color_manager_v1_create(struct wl_display *display,
		uint32_t version, const struct nx_color_manager_v1_options *options) {
	assert(version <= COLOR_MANAGEMENT_V1_VERSION);
	assert_valid_options(options);

	struct nx_color_manager_v1 *manager = calloc(1, sizeof(*manager));
	if (manager == NULL) {
		return NULL;
	}

	if (!copy_options(manager, options)) {
		manager_free(manager);
		return NULL;
	}

	wl_signal_init(&manager->events.destroy);
	wl_list_init(&manager->surface_feedbacks);

	manager->global = wl_global_create(display, &wp_color_manager_v1_interface,
		version, manager, manager_bind);
	if (manager->global == NULL) {
		manager_free(manager);
		return NULL;
	}

	manager->display_destroy.notify = manager_handle_display_destroy;
	wl_display_add_destroy_listener(display, &manager->display_destroy);

	return manager;
}
