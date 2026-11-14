#include "color-management-v1-protocol.h"
#include "platform/surface/color/parametric_creator.h"

void color_params_set_mastering_display_primaries(
		struct wl_client *client, struct wl_resource *params_resource,
		int32_t r_x, int32_t r_y, int32_t g_x, int32_t g_y,
		int32_t b_x, int32_t b_y, int32_t w_x, int32_t w_y) {
	struct color_params *params = color_params_from_resource(params_resource);

	if (!params->manager->features.set_mastering_display_primaries) {
		wl_resource_post_error(params_resource,
			WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_UNSUPPORTED_FEATURE,
			"set_mastering_display_primaries is not supported");
		return;
	}
	if (params->data.has_mastering_display_primaries) {
		wl_resource_post_error(params_resource,
			WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_ALREADY_SET,
			"mastering display primaries already set");
		return;
	}

	params->data.has_mastering_display_primaries = true;
	params->data.mastering_display_primaries = (struct nx_color_primaries){
		.red = color_params_decode_xy(r_x, r_y),
		.green = color_params_decode_xy(g_x, g_y),
		.blue = color_params_decode_xy(b_x, b_y),
		.white = color_params_decode_xy(w_x, w_y),
	};
}

void color_params_set_mastering_luminance(struct wl_client *client,
		struct wl_resource *params_resource, uint32_t min_lum, uint32_t max_lum) {
	struct color_params *params = color_params_from_resource(params_resource);

	if (!params->manager->features.set_mastering_display_primaries) {
		wl_resource_post_error(params_resource,
			WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_UNSUPPORTED_FEATURE,
			"set_mastering_luminance is not supported");
		return;
	}
	if (params->data.has_mastering_luminance) {
		wl_resource_post_error(params_resource,
			WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_ALREADY_SET,
			"mastering luminance already set");
		return;
	}

	params->data.has_mastering_luminance = true;
	params->data.mastering_luminance.min = (float)min_lum / 10000;
	params->data.mastering_luminance.max = (float)max_lum;

	if (params->data.mastering_luminance.max <= params->data.mastering_luminance.min) {
		wl_resource_post_error(params_resource,
			WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INVALID_LUMINANCE,
			"max luminance must be greater than min luminance");
	}
}

void color_params_set_max_cll(struct wl_client *client,
		struct wl_resource *params_resource, uint32_t max_cll) {
	color_params_from_resource(params_resource)->data.max_cll = max_cll;
}

void color_params_set_max_fall(struct wl_client *client,
		struct wl_resource *params_resource, uint32_t max_fall) {
	color_params_from_resource(params_resource)->data.max_fall = max_fall;
}
