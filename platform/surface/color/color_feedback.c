#include <assert.h>
#include <stdlib.h>
#include "color-management-v1-protocol.h"
#include "platform/surface/color/image_description.h"
#include "platform/surface/color/manager_requests.h"
#include "platform/surface/compositor.h"

struct color_feedback {
	struct wl_resource *resource;
	struct nx_surface *surface;
	struct nx_color_manager_v1 *manager;
	struct wl_list link; // nx_color_manager_v1.surface_feedbacks

	struct nx_image_description_v1_data data;

	struct wl_listener surface_destroy;
};

static bool cie1931_xy_equal(const struct nx_color_cie1931_xy *a,
		const struct nx_color_cie1931_xy *b) {
	return a->x == b->x && a->y == b->y;
}

static bool primaries_equal(const struct nx_color_primaries *a,
		const struct nx_color_primaries *b) {
	return cie1931_xy_equal(&a->red, &b->red) &&
		cie1931_xy_equal(&a->green, &b->green) &&
		cie1931_xy_equal(&a->blue, &b->blue) &&
		cie1931_xy_equal(&a->white, &b->white);
}

static bool img_desc_data_equal(const struct nx_image_description_v1_data *a,
		const struct nx_image_description_v1_data *b) {
	if (a->tf_named != b->tf_named ||
			a->primaries_named != b->primaries_named ||
			a->has_mastering_display_primaries != b->has_mastering_display_primaries ||
			a->has_mastering_luminance != b->has_mastering_luminance ||
			a->max_cll != b->max_cll ||
			a->max_fall != b->max_fall) {
		return false;
	}
	if (a->has_mastering_display_primaries &&
			!primaries_equal(&a->mastering_display_primaries, &b->mastering_display_primaries)) {
		return false;
	}
	return !a->has_mastering_luminance ||
		(a->mastering_luminance.min == b->mastering_luminance.min &&
		a->mastering_luminance.max == b->mastering_luminance.max);
}

static const struct wp_color_management_surface_feedback_v1_interface color_feedback_impl;

static struct color_feedback *color_feedback_from_resource(struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &wp_color_management_surface_feedback_v1_interface,
		&color_feedback_impl));
	return wl_resource_get_user_data(resource);
}

static void color_feedback_send_preferred(struct wl_resource *feedback_resource,
		uint32_t id, const char *request) {
	struct color_feedback *color_feedback = color_feedback_from_resource(feedback_resource);
	if (color_feedback == NULL) {
		wl_resource_post_error(feedback_resource,
			WP_COLOR_MANAGEMENT_SURFACE_FEEDBACK_V1_ERROR_INERT,
			"%s sent on inert feedback surface", request);
		return;
	}

	const struct color_image_desc_ready ready = {
		.manager = color_feedback->manager,
		.data = &color_feedback->data,
		.get_info_allowed = true,
	};
	color_image_desc_create_ready(feedback_resource, id, &ready);
}

static void color_feedback_handle_get_preferred(struct wl_client *client,
		struct wl_resource *feedback_resource, uint32_t id) {
	color_feedback_send_preferred(feedback_resource, id, "get_preferred");
}

static void color_feedback_handle_get_preferred_parametric(struct wl_client *client,
		struct wl_resource *feedback_resource, uint32_t id) {
	color_feedback_send_preferred(feedback_resource, id, "get_preferred_parametric");
}

static void color_feedback_handle_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct wp_color_management_surface_feedback_v1_interface color_feedback_impl = {
	.destroy = color_feedback_handle_destroy,
	.get_preferred = color_feedback_handle_get_preferred,
	.get_preferred_parametric = color_feedback_handle_get_preferred_parametric,
};

static void color_feedback_destroy(struct color_feedback *color_feedback) {
	if (color_feedback == NULL) {
		return;
	}
	wl_resource_set_user_data(color_feedback->resource, NULL);
	wl_list_remove(&color_feedback->surface_destroy.link);
	wl_list_remove(&color_feedback->link);
	free(color_feedback);
}

static void color_feedback_handle_resource_destroy(struct wl_resource *resource) {
	color_feedback_destroy(color_feedback_from_resource(resource));
}

static void color_feedback_handle_surface_destroy(struct wl_listener *listener, void *data) {
	struct color_feedback *color_feedback =
		wl_container_of(listener, color_feedback, surface_destroy);
	color_feedback_destroy(color_feedback);
}

void color_manager_get_surface_feedback(struct wl_client *client,
		struct wl_resource *manager_resource, uint32_t id,
		struct wl_resource *surface_resource) {
	struct nx_color_manager_v1 *manager = color_manager_from_resource(manager_resource);
	struct nx_surface *surface = nx_surface_from_resource(surface_resource);

	struct color_feedback *color_feedback = calloc(1, sizeof(*color_feedback));
	if (color_feedback == NULL) {
		wl_client_post_no_memory(client);
		return;
	}

	color_feedback->manager = manager;

	uint32_t version = wl_resource_get_version(manager_resource);
	color_feedback->resource = wl_resource_create(client,
		&wp_color_management_surface_feedback_v1_interface, version, id);
	if (!color_feedback->resource) {
		wl_client_post_no_memory(client);
		free(color_feedback);
		return;
	}
	wl_resource_set_implementation(color_feedback->resource, &color_feedback_impl,
		color_feedback, color_feedback_handle_resource_destroy);

	color_feedback->surface = surface;
	color_feedback->data = (struct nx_image_description_v1_data){
		.tf_named = WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22,
		.primaries_named = WP_COLOR_MANAGER_V1_PRIMARIES_SRGB,
	};

	color_feedback->surface_destroy.notify = color_feedback_handle_surface_destroy;
	wl_signal_add(&surface->events.destroy, &color_feedback->surface_destroy);

	wl_list_insert(&manager->surface_feedbacks, &color_feedback->link);
}

void nx_color_manager_v1_set_surface_preferred_image_description(
		struct nx_color_manager_v1 *manager, struct nx_surface *surface,
		const struct nx_image_description_v1_data *data) {
	uint32_t identity = (uint32_t)++manager->last_image_desc_identity;

	struct color_feedback *color_feedback;
	wl_list_for_each(color_feedback, &manager->surface_feedbacks, link) {
		if (color_feedback->surface != surface ||
				img_desc_data_equal(&color_feedback->data, data)) {
			continue;
		}

		color_feedback->data = *data;
		wp_color_management_surface_feedback_v1_send_preferred_changed(
			color_feedback->resource, identity);
	}
}
