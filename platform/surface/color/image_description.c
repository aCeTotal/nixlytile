#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include "color-management-v1-protocol.h"
#include "platform/render/color.h"
#include "platform/surface/color/image_description.h"

struct nx_image_description_v1 {
	bool get_info_allowed;
	struct nx_image_description_v1_data data; // immutable
};

typedef void (*send_primaries_func)(struct wl_resource *resource,
	int32_t r_x, int32_t r_y, int32_t g_x, int32_t g_y,
	int32_t b_x, int32_t b_y, int32_t w_x, int32_t w_y);

static const struct wp_image_description_v1_interface image_desc_impl;

static struct nx_image_description_v1 *image_desc_from_resource(struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &wp_image_description_v1_interface, &image_desc_impl));
	return wl_resource_get_user_data(resource);
}

const struct nx_image_description_v1_data *color_image_desc_data_from_resource(
		struct wl_resource *resource) {
	struct nx_image_description_v1 *image_desc = image_desc_from_resource(resource);
	if (image_desc == NULL) {
		return NULL;
	}
	return &image_desc->data;
}

static int32_t encode_cie1931_coord(float value) {
	return (int32_t)round(value * 1000 * 1000);
}

static void send_encoded_primaries(struct wl_resource *resource,
		send_primaries_func send, const struct nx_color_primaries *primaries) {
	send(resource,
		encode_cie1931_coord(primaries->red.x), encode_cie1931_coord(primaries->red.y),
		encode_cie1931_coord(primaries->green.x), encode_cie1931_coord(primaries->green.y),
		encode_cie1931_coord(primaries->blue.x), encode_cie1931_coord(primaries->blue.y),
		encode_cie1931_coord(primaries->white.x), encode_cie1931_coord(primaries->white.y));
}

static void resolve_primaries(const struct nx_image_description_v1_data *data,
		struct nx_color_primaries *primaries) {
	if (data->has_primaries) {
		*primaries = data->primaries;
		return;
	}
	if (data->primaries_named != 0) {
		nx_color_primaries_from_named(primaries,
			nx_color_manager_v1_primaries_to_nx(data->primaries_named));
		return;
	}
	nx_color_primaries_from_named(primaries, NX_COLOR_NAMED_PRIMARIES_SRGB);
}

static void resolve_luminances(const struct nx_image_description_v1_data *data,
		struct nx_color_luminances *luminances) {
	if (data->has_luminances) {
		*luminances = data->luminances;
		return;
	}
	nx_color_transfer_function_get_default_luminance(
		nx_color_manager_v1_transfer_function_to_nx(data->tf_named), luminances);
}

static void send_target_luminance(struct wl_resource *resource,
		const struct nx_image_description_v1_data *data,
		const struct nx_color_luminances *luminances) {
	if (data->has_mastering_luminance) {
		wp_image_description_info_v1_send_target_luminance(resource,
			(uint32_t)round(data->mastering_luminance.min * 10000),
			(uint32_t)round(data->mastering_luminance.max));
		return;
	}
	wp_image_description_info_v1_send_target_luminance(resource,
		(uint32_t)round(luminances->min * 10000), (uint32_t)round(luminances->max));
}

static void send_information(struct wl_resource *resource,
		const struct nx_image_description_v1_data *data) {
	struct nx_color_primaries primaries;
	resolve_primaries(data, &primaries);
	struct nx_color_luminances luminances;
	resolve_luminances(data, &luminances);

	if (data->primaries_named != 0) {
		wp_image_description_info_v1_send_primaries_named(resource, data->primaries_named);
	}
	send_encoded_primaries(resource, wp_image_description_info_v1_send_primaries,
		&primaries);
	wp_image_description_info_v1_send_tf_named(resource, data->tf_named);
	wp_image_description_info_v1_send_luminances(resource,
		(uint32_t)round(luminances.min * 10000), (uint32_t)round(luminances.max),
		(uint32_t)round(luminances.reference));
	send_encoded_primaries(resource, wp_image_description_info_v1_send_target_primaries,
		data->has_mastering_display_primaries ?
			&data->mastering_display_primaries : &primaries);
	send_target_luminance(resource, data, &luminances);
	wp_image_description_info_v1_send_done(resource);
}

static void image_desc_handle_get_information(struct wl_client *client,
		struct wl_resource *image_desc_resource, uint32_t id) {
	struct nx_image_description_v1 *image_desc = image_desc_from_resource(image_desc_resource);
	if (image_desc == NULL) {
		wl_resource_post_error(image_desc_resource,
			WP_IMAGE_DESCRIPTION_V1_ERROR_NOT_READY,
			"image description is in failed state");
		return;
	}

	if (!image_desc->get_info_allowed) {
		wl_resource_post_error(image_desc_resource,
			WP_IMAGE_DESCRIPTION_V1_ERROR_NO_INFORMATION,
			"get_information not allowed");
		return;
	}

	uint32_t version = wl_resource_get_version(image_desc_resource);
	struct wl_resource *resource = wl_resource_create(client,
		&wp_image_description_info_v1_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}

	send_information(resource, &image_desc->data);
	wl_resource_destroy(resource);
}

static void image_desc_handle_destroy(struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct wp_image_description_v1_interface image_desc_impl = {
	.destroy = image_desc_handle_destroy,
	.get_information = image_desc_handle_get_information,
};

static void image_desc_handle_resource_destroy(struct wl_resource *resource) {
	free(image_desc_from_resource(resource));
}

static struct wl_resource *image_desc_create_resource(
		struct wl_resource *parent_resource, uint32_t id) {
	struct wl_client *client = wl_resource_get_client(parent_resource);
	uint32_t version = wl_resource_get_version(parent_resource);
	return wl_resource_create(client, &wp_image_description_v1_interface,
		version, id);
}

void color_image_desc_create_ready(struct wl_resource *parent_resource, uint32_t id,
		const struct color_image_desc_ready *ready) {
	struct nx_image_description_v1 *image_desc = calloc(1, sizeof(*image_desc));
	if (image_desc == NULL) {
		wl_resource_post_no_memory(parent_resource);
		return;
	}

	image_desc->data = *ready->data;
	image_desc->get_info_allowed = ready->get_info_allowed;

	struct wl_resource *resource = image_desc_create_resource(parent_resource, id);
	if (!resource) {
		wl_resource_post_no_memory(parent_resource);
		free(image_desc);
		return;
	}
	wl_resource_set_implementation(resource, &image_desc_impl,
		image_desc, image_desc_handle_resource_destroy);

	uint32_t version = wl_resource_get_version(resource);
	if (!wp_color_manager_v1_transfer_function_is_valid(ready->data->tf_named, version)) {
		wp_image_description_v1_send_failed(resource,
			WP_IMAGE_DESCRIPTION_V1_CAUSE_LOW_VERSION, "unhandled value for tf_named");
		return;
	}

	uint32_t identity = (uint32_t)++ready->manager->last_image_desc_identity;
	wp_image_description_v1_send_ready(resource, identity);
}

void color_image_desc_create_failed(struct wl_resource *parent_resource, uint32_t id,
		const struct color_image_desc_failure *failure) {
	struct wl_resource *resource = image_desc_create_resource(parent_resource, id);
	if (resource == NULL) {
		wl_resource_post_no_memory(parent_resource);
		return;
	}
	wl_resource_set_implementation(resource, &image_desc_impl, NULL, NULL);

	wp_image_description_v1_send_failed(resource, failure->cause, failure->msg);
}
