#include <assert.h>
#include <stdlib.h>
#include "color-management-v1-protocol.h"
#include "platform/surface/color/image_description.h"
#include "platform/surface/color/manager_requests.h"
#include "platform/surface/color/parametric_creator.h"

static const struct wp_image_description_creator_params_v1_interface params_impl;

struct color_params *color_params_from_resource(struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource,
		&wp_image_description_creator_params_v1_interface, &params_impl));
	return wl_resource_get_user_data(resource);
}

static float decode_cie1931_coord(int32_t raw) {
	return (float)raw / (1000 * 1000);
}

struct nx_color_cie1931_xy color_params_decode_xy(int32_t raw_x, int32_t raw_y) {
	return (struct nx_color_cie1931_xy){
		decode_cie1931_coord(raw_x), decode_cie1931_coord(raw_y),
	};
}

static bool check_mastering_luminance_range(struct wl_resource *params_resource,
		const struct nx_image_description_v1_data *data,
		float value, const char *name) {
	if (!data->has_mastering_luminance || value == 0) {
		return true;
	}

	if (value <= data->mastering_luminance.min) {
		wl_resource_post_error(params_resource,
			WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INVALID_LUMINANCE,
			"%s must be greater than min L of the mastering luminance range",
			name);
		return false;
	}
	if (value > data->mastering_luminance.max) {
		wl_resource_post_error(params_resource,
			WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INVALID_LUMINANCE,
			"%s must be less or equal to max L of the mastering luminance range",
			name);
		return false;
	}

	return true;
}

// Posts the first violation
static bool check_complete(struct wl_resource *params_resource,
		const struct nx_image_description_v1_data *data) {
	if (data->tf_named == 0) {
		wl_resource_post_error(params_resource,
			WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INCOMPLETE_SET,
			"missing transfer function");
		return false;
	}
	if (data->primaries_named == 0 && !data->has_primaries) {
		wl_resource_post_error(params_resource,
			WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INCOMPLETE_SET,
			"missing primaries");
		return false;
	}
	if (data->max_cll != 0 && data->max_fall != 0 && data->max_fall > data->max_cll) {
		wl_resource_post_error(params_resource,
			WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INVALID_LUMINANCE,
			"max_fall must be less or equal to max_cll");
		return false;
	}
	return check_mastering_luminance_range(params_resource, data,
			(float)data->max_cll, "max_cll") &&
		check_mastering_luminance_range(params_resource, data,
			(float)data->max_fall, "max_fall");
}

static void params_handle_create(struct wl_client *client,
		struct wl_resource *params_resource, uint32_t id) {
	struct color_params *params = color_params_from_resource(params_resource);
	if (!check_complete(params_resource, &params->data)) {
		return;
	}

	const struct color_image_desc_ready ready = {
		.manager = params->manager,
		.data = &params->data,
		.get_info_allowed = false,
	};
	color_image_desc_create_ready(params_resource, id, &ready);
}

static bool contains_tf(const struct nx_color_manager_v1 *manager, uint32_t tf) {
	for (size_t i = 0; i < manager->transfer_functions_len; i++) {
		if (manager->transfer_functions[i] == tf) {
			return true;
		}
	}
	return false;
}

static bool contains_primaries(const struct nx_color_manager_v1 *manager, uint32_t primaries) {
	for (size_t i = 0; i < manager->primaries_len; i++) {
		if (manager->primaries[i] == primaries) {
			return true;
		}
	}
	return false;
}

static void params_handle_set_tf_named(struct wl_client *client,
		struct wl_resource *params_resource, uint32_t tf) {
	struct color_params *params = color_params_from_resource(params_resource);

	if (params->data.tf_named != 0) {
		wl_resource_post_error(params_resource,
			WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_ALREADY_SET,
			"transfer function already set");
		return;
	}
	if (!contains_tf(params->manager, tf)) {
		wl_resource_post_error(params_resource,
			WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INVALID_TF,
			"invalid transfer function");
		return;
	}

	params->data.tf_named = tf;
}

static void params_handle_set_tf_power(struct wl_client *client,
		struct wl_resource *params_resource, uint32_t eexp) {
	wl_resource_post_error(params_resource,
		WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_UNSUPPORTED_FEATURE,
		"set_tf_power is not supported");
}

static void params_handle_set_primaries_named(struct wl_client *client,
		struct wl_resource *params_resource, uint32_t primaries) {
	struct color_params *params = color_params_from_resource(params_resource);

	if (params->data.primaries_named != 0) {
		wl_resource_post_error(params_resource,
			WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_ALREADY_SET,
			"primaries already set");
		return;
	}
	if (!contains_primaries(params->manager, primaries)) {
		wl_resource_post_error(params_resource,
			WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INVALID_PRIMARIES_NAMED,
			"invalid primaries");
		return;
	}

	params->data.primaries_named = primaries;
}

static void params_handle_set_primaries(struct wl_client *client,
		struct wl_resource *params_resource, int32_t r_x, int32_t r_y,
		int32_t g_x, int32_t g_y, int32_t b_x, int32_t b_y,
		int32_t w_x, int32_t w_y) {
	struct color_params *params = color_params_from_resource(params_resource);

	if (!params->manager->features.set_primaries) {
		wl_resource_post_error(params_resource,
			WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_UNSUPPORTED_FEATURE,
			"set_primaries is not supported");
		return;
	}
	if (params->data.has_primaries || params->data.primaries_named != 0) {
		wl_resource_post_error(params_resource,
			WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_ALREADY_SET,
			"primaries already set");
		return;
	}

	params->data.has_primaries = true;
	params->data.primaries = (struct nx_color_primaries){
		.red = color_params_decode_xy(r_x, r_y),
		.green = color_params_decode_xy(g_x, g_y),
		.blue = color_params_decode_xy(b_x, b_y),
		.white = color_params_decode_xy(w_x, w_y),
	};
}

static void params_handle_set_luminances(struct wl_client *client,
		struct wl_resource *params_resource, uint32_t min_lum,
		uint32_t max_lum, uint32_t reference_lum) {
	struct color_params *params = color_params_from_resource(params_resource);

	if (!params->manager->features.set_luminances) {
		wl_resource_post_error(params_resource,
			WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_UNSUPPORTED_FEATURE,
			"set_luminances is not supported");
		return;
	}
	if (params->data.has_luminances) {
		wl_resource_post_error(params_resource,
			WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_ALREADY_SET,
			"luminances already set");
		return;
	}

	struct nx_color_luminances luminances = {
		.min = (float)min_lum / 10000.0f,
		.max = (float)max_lum,
		.reference = (float)reference_lum,
	};
	if (luminances.max <= luminances.min || luminances.reference <= luminances.min) {
		wl_resource_post_error(params_resource,
			WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INVALID_LUMINANCE,
			"max and reference luminance must be greater than min luminance");
		return;
	}

	params->data.has_luminances = true;
	params->data.luminances = luminances;
}

static const struct wp_image_description_creator_params_v1_interface params_impl = {
	.create = params_handle_create,
	.set_tf_named = params_handle_set_tf_named,
	.set_tf_power = params_handle_set_tf_power,
	.set_primaries_named = params_handle_set_primaries_named,
	.set_primaries = params_handle_set_primaries,
	.set_luminances = params_handle_set_luminances,
	.set_mastering_display_primaries = color_params_set_mastering_display_primaries,
	.set_mastering_luminance = color_params_set_mastering_luminance,
	.set_max_cll = color_params_set_max_cll,
	.set_max_fall = color_params_set_max_fall,
};

static void params_handle_resource_destroy(struct wl_resource *resource) {
	free(color_params_from_resource(resource));
}

void color_manager_create_parametric_creator(struct wl_client *client,
		struct wl_resource *manager_resource, uint32_t id) {
	struct nx_color_manager_v1 *manager = color_manager_from_resource(manager_resource);
	if (!manager->features.parametric) {
		wl_resource_post_error(manager_resource,
			WP_COLOR_MANAGER_V1_ERROR_UNSUPPORTED_FEATURE,
			"new_parametric_creator is not supported");
		return;
	}

	struct color_params *params = calloc(1, sizeof(*params));
	if (params == NULL) {
		wl_client_post_no_memory(client);
		return;
	}

	params->manager = manager;

	uint32_t version = wl_resource_get_version(manager_resource);
	struct wl_resource *resource = wl_resource_create(client,
		&wp_image_description_creator_params_v1_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		free(params);
		return;
	}
	wl_resource_set_implementation(resource, &params_impl,
		params, params_handle_resource_destroy);
}
