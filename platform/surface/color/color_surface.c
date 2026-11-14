#include <assert.h>
#include <stdlib.h>
#include "color-management-v1-protocol.h"
#include "platform/surface/color/image_description.h"
#include "platform/surface/color/manager_requests.h"
#include "platform/surface/compositor.h"

struct color_surface_state {
	bool has_image_desc_data;
	struct nx_image_description_v1_data image_desc_data;
};

struct color_surface {
	struct wl_resource *resource;
	struct nx_color_manager_v1 *manager;

	struct nx_addon addon;
	struct nx_surface_synced synced;

	struct color_surface_state current, pending;
};

static void color_surface_destroy(struct color_surface *color_surface) {
	if (color_surface == NULL) {
		return;
	}
	wl_resource_set_user_data(color_surface->resource, NULL);
	nx_surface_synced_finish(&color_surface->synced);
	nx_addon_finish(&color_surface->addon);
	free(color_surface);
}

static const struct nx_surface_synced_impl color_surface_synced_impl = {
	.state_size = sizeof(struct color_surface_state),
};

static void color_surface_handle_addon_destroy(struct nx_addon *addon) {
	struct color_surface *color_surface = wl_container_of(addon, color_surface, addon);
	color_surface_destroy(color_surface);
}

static const struct nx_addon_interface color_surface_addon_impl = {
	.name = "nx_color_management_surface_v1",
	.destroy = color_surface_handle_addon_destroy,
};

static const struct wp_color_management_surface_v1_interface color_surface_impl;

static struct color_surface *color_surface_from_resource(struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &wp_color_management_surface_v1_interface,
		&color_surface_impl));
	return wl_resource_get_user_data(resource);
}

// NULL after posting inert error
static struct color_surface *live_color_surface_from_resource(struct wl_resource *resource) {
	struct color_surface *color_surface = color_surface_from_resource(resource);
	if (color_surface == NULL) {
		wl_resource_post_error(resource,
			WP_COLOR_MANAGEMENT_SURFACE_V1_ERROR_INERT,
			"set_image_description cannot be sent on an inert object");
	}
	return color_surface;
}

static bool supports_render_intent(const struct nx_color_manager_v1 *manager,
		uint32_t render_intent) {
	for (size_t i = 0; i < manager->render_intents_len; i++) {
		if (manager->render_intents[i] == render_intent) {
			return true;
		}
	}
	return false;
}

static void color_surface_handle_set_image_description(struct wl_client *client,
		struct wl_resource *color_surface_resource,
		struct wl_resource *image_desc_resource, uint32_t render_intent) {
	struct color_surface *color_surface =
		live_color_surface_from_resource(color_surface_resource);
	if (color_surface == NULL) {
		return;
	}

	const struct nx_image_description_v1_data *data =
		color_image_desc_data_from_resource(image_desc_resource);
	if (data == NULL) {
		wl_resource_post_error(color_surface_resource,
			WP_COLOR_MANAGEMENT_SURFACE_V1_ERROR_IMAGE_DESCRIPTION,
			"Image description to be set is invalid");
		return;
	}

	if (!supports_render_intent(color_surface->manager, render_intent)) {
		wl_resource_post_error(color_surface_resource,
			WP_COLOR_MANAGEMENT_SURFACE_V1_ERROR_RENDER_INTENT,
			"invalid render intent");
		return;
	}

	color_surface->pending.has_image_desc_data = true;
	color_surface->pending.image_desc_data = *data;
}

static void color_surface_handle_unset_image_description(struct wl_client *client,
		struct wl_resource *color_surface_resource) {
	struct color_surface *color_surface =
		live_color_surface_from_resource(color_surface_resource);
	if (color_surface == NULL) {
		return;
	}

	color_surface->pending.has_image_desc_data = false;
}

static void color_surface_handle_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct wp_color_management_surface_v1_interface color_surface_impl = {
	.destroy = color_surface_handle_destroy,
	.set_image_description = color_surface_handle_set_image_description,
	.unset_image_description = color_surface_handle_unset_image_description,
};

static void color_surface_handle_resource_destroy(struct wl_resource *resource) {
	color_surface_destroy(color_surface_from_resource(resource));
}

static struct color_surface *color_surface_from_surface(struct nx_surface *surface) {
	struct nx_addon *addon = nx_addon_find(&surface->addons, NULL, &color_surface_addon_impl);
	if (addon == NULL) {
		return NULL;
	}
	struct color_surface *color_surface = wl_container_of(addon, color_surface, addon);
	return color_surface;
}

void color_manager_get_surface(struct wl_client *client,
		struct wl_resource *manager_resource, uint32_t id,
		struct wl_resource *surface_resource) {
	struct nx_color_manager_v1 *manager = color_manager_from_resource(manager_resource);
	struct nx_surface *surface = nx_surface_from_resource(surface_resource);

	if (color_surface_from_surface(surface) != NULL) {
		wl_resource_post_error(manager_resource,
			WP_COLOR_MANAGER_V1_ERROR_SURFACE_EXISTS,
			"wp_color_management_surface_v1 already constructed for this surface");
		return;
	}

	struct color_surface *color_surface = calloc(1, sizeof(*color_surface));
	if (color_surface == NULL) {
		wl_client_post_no_memory(client);
		return;
	}

	if (!nx_surface_synced_init(&color_surface->synced, surface, &color_surface_synced_impl,
			&color_surface->pending, &color_surface->current)) {
		wl_client_post_no_memory(client);
		free(color_surface);
		return;
	}

	uint32_t version = wl_resource_get_version(manager_resource);
	color_surface->resource = wl_resource_create(client,
		&wp_color_management_surface_v1_interface, version, id);
	if (!color_surface->resource) {
		wl_client_post_no_memory(client);
		nx_surface_synced_finish(&color_surface->synced);
		free(color_surface);
		return;
	}
	wl_resource_set_implementation(color_surface->resource, &color_surface_impl,
		color_surface, color_surface_handle_resource_destroy);

	color_surface->manager = manager;

	nx_addon_init(&color_surface->addon, &surface->addons, NULL, &color_surface_addon_impl);
}

const struct nx_image_description_v1_data *
nx_surface_get_image_description_v1_data(struct nx_surface *surface) {
	struct color_surface *color_surface = color_surface_from_surface(surface);
	if (color_surface == NULL || !color_surface->current.has_image_desc_data) {
		return NULL;
	}
	return &color_surface->current.image_desc_data;
}
