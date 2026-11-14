#include <assert.h>
#include <drm_fourcc.h>
#include <stdlib.h>
#include "color-representation-v1-protocol.h"
#include "platform/buffer/buffer.h"
#include "platform/container/addon.h"
#include "platform/render/pixel_format.h"
#include "platform/surface/color/representation_manager.h"
#include "platform/surface/compositor.h"

struct color_repr {
	struct wl_resource *resource;
	struct nx_surface *surface;
	struct nx_color_representation_manager_v1 *manager;

	struct nx_addon addon;
	struct nx_surface_synced synced;
	struct nx_color_representation_v1_surface_state pending, current;
};

static const struct wp_color_representation_surface_v1_interface color_repr_impl;

static struct color_repr *color_repr_from_resource(struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource,
		&wp_color_representation_surface_v1_interface,
		&color_repr_impl));
	return wl_resource_get_user_data(resource);
}

// NULL after posting inert error
static struct color_repr *live_color_repr_from_resource(struct wl_resource *resource) {
	struct color_repr *color_repr = color_repr_from_resource(resource);
	if (color_repr == NULL) {
		wl_resource_post_error(resource, WP_COLOR_REPRESENTATION_SURFACE_V1_ERROR_INERT,
			"Associated surface has been destroyed, object is inert");
	}
	return color_repr;
}

static void color_repr_handle_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static void color_repr_handle_set_alpha_mode(struct wl_client *client,
		struct wl_resource *resource, uint32_t alpha_mode) {
	if (live_color_repr_from_resource(resource) == NULL) {
		return;
	}
	if (alpha_mode != COLOR_REPR_ALPHA_MODE) {
		wl_resource_post_error(resource,
			WP_COLOR_REPRESENTATION_SURFACE_V1_ERROR_ALPHA_MODE,
			"Unsupported alpha mode");
	}
}

static void color_repr_handle_set_coefficients_and_range(struct wl_client *client,
		struct wl_resource *resource, uint32_t coefficients,
		uint32_t range) {
	struct color_repr *color_repr = live_color_repr_from_resource(resource);
	if (color_repr == NULL) {
		return;
	}

	if (!color_repr_manager_supports(color_repr->manager, coefficients, range)) {
		wl_resource_post_error(resource,
			WP_COLOR_REPRESENTATION_SURFACE_V1_ERROR_COEFFICIENTS,
			"Unsupported coefficients/range pair");
		return;
	}

	color_repr->pending.coefficients = coefficients;
	color_repr->pending.range = range;
}

// Unknown locations are ignored
static void color_repr_handle_set_chroma_location(struct wl_client *client,
		struct wl_resource *resource, uint32_t chroma_location) {
	if (live_color_repr_from_resource(resource) == NULL) {
		return;
	}

	uint32_t version = wl_resource_get_version(resource);
	if (!wp_color_representation_surface_v1_chroma_location_is_valid(
			version, chroma_location)) {
		wl_resource_post_error(resource, WP_COLOR_REPRESENTATION_SURFACE_V1_ERROR_CHROMA_LOCATION,
			"Chroma location is not a valid enum value");
	}
}

static const struct wp_color_representation_surface_v1_interface color_repr_impl = {
	.destroy = color_repr_handle_destroy,
	.set_alpha_mode = color_repr_handle_set_alpha_mode,
	.set_coefficients_and_range = color_repr_handle_set_coefficients_and_range,
	.set_chroma_location = color_repr_handle_set_chroma_location,
};

static void color_repr_destroy(struct color_repr *color_repr) {
	if (color_repr == NULL) {
		return;
	}
	nx_surface_synced_finish(&color_repr->synced);
	nx_addon_finish(&color_repr->addon);
	wl_resource_set_user_data(color_repr->resource, NULL);
	free(color_repr);
}

static void color_repr_addon_destroy(struct nx_addon *addon) {
	struct color_repr *color_repr = wl_container_of(addon, color_repr, addon);
	color_repr_destroy(color_repr);
}

static const struct nx_addon_interface surface_addon_impl = {
	.name = "nx_color_representation_v1",
	.destroy = color_repr_addon_destroy,
};

static void color_repr_handle_resource_destroy(struct wl_resource *resource) {
	color_repr_destroy(color_repr_from_resource(resource));
}

static uint32_t committed_drm_format(const struct nx_surface *surface) {
	if (surface->buffer == NULL) {
		return DRM_FORMAT_INVALID;
	}
	return buffer_get_drm_format(&surface->buffer->base);
}

static void color_repr_synced_commit(struct nx_surface_synced *synced) {
	struct color_repr *color_repr = wl_container_of(synced, color_repr, synced);
	const struct nx_color_representation_v1_surface_state *current = &color_repr->current;

	if (current->coefficients == 0 && current->range == 0) {
		return;
	}

	uint32_t drm_format = committed_drm_format(color_repr->surface);
	if (drm_format == DRM_FORMAT_INVALID) {
		return;
	}

	bool is_ycbcr = pixel_format_is_ycbcr(drm_format);
	bool is_identity_full =
		current->coefficients == WP_COLOR_REPRESENTATION_SURFACE_V1_COEFFICIENTS_IDENTITY &&
		current->range == WP_COLOR_REPRESENTATION_SURFACE_V1_RANGE_FULL;

	if (is_ycbcr && is_identity_full) {
		wl_resource_post_error(color_repr->resource,
			WP_COLOR_REPRESENTATION_SURFACE_V1_ERROR_PIXEL_FORMAT,
			"unexpected encoding/range for yuv");
		return;
	}
	if (!is_ycbcr && !is_identity_full) {
		wl_resource_post_error(color_repr->resource,
			WP_COLOR_REPRESENTATION_SURFACE_V1_ERROR_PIXEL_FORMAT,
			"unexpected encoding/range for rgb");
	}
}

static const struct nx_surface_synced_impl surface_synced_impl = {
	.state_size = sizeof(struct nx_color_representation_v1_surface_state),
	.commit = color_repr_synced_commit,
};

static struct color_repr *color_repr_from_surface(struct nx_surface *surface) {
	struct nx_addon *addon = nx_addon_find(&surface->addons, NULL, &surface_addon_impl);
	if (addon == NULL) {
		return NULL;
	}
	struct color_repr *color_repr = wl_container_of(addon, color_repr, addon);
	return color_repr;
}

void color_repr_manager_get_surface(struct wl_client *client,
		struct wl_resource *manager_resource, uint32_t id,
		struct wl_resource *surface_resource) {
	struct nx_surface *surface = nx_surface_from_resource(surface_resource);

	if (color_repr_from_surface(surface) != NULL) {
		wl_resource_post_error(manager_resource,
			WP_COLOR_REPRESENTATION_MANAGER_V1_ERROR_SURFACE_EXISTS,
			"wp_color_representation_surface_v1 already exists for this surface");
		return;
	}

	struct color_repr *color_repr = calloc(1, sizeof(*color_repr));
	if (!color_repr) {
		wl_resource_post_no_memory(manager_resource);
		return;
	}

	color_repr->manager = color_repr_manager_from_resource(manager_resource);
	color_repr->surface = surface;

	if (!nx_surface_synced_init(&color_repr->synced, surface,
			&surface_synced_impl, &color_repr->pending, &color_repr->current)) {
		free(color_repr);
		wl_resource_post_no_memory(manager_resource);
		return;
	}

	uint32_t version = wl_resource_get_version(manager_resource);
	color_repr->resource = wl_resource_create(client,
		&wp_color_representation_surface_v1_interface, version, id);
	if (color_repr->resource == NULL) {
		nx_surface_synced_finish(&color_repr->synced);
		free(color_repr);
		wl_resource_post_no_memory(manager_resource);
		return;
	}
	wl_resource_set_implementation(color_repr->resource,
		&color_repr_impl, color_repr, color_repr_handle_resource_destroy);

	nx_addon_init(&color_repr->addon, &surface->addons, NULL, &surface_addon_impl);
}

const struct nx_color_representation_v1_surface_state *nx_color_representation_v1_get_surface_state(
		struct nx_surface *surface) {
	struct color_repr *color_repr = color_repr_from_surface(surface);
	if (color_repr == NULL) {
		return NULL;
	}
	return &color_repr->current;
}
