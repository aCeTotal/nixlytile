#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include "platform/geometry/transform.h"
#include "platform/surface/compositor.h"
#include "platform/surface/viewporter.h"
#include "viewporter-protocol.h"

#define VIEWPORTER_VERSION 1

struct nx_viewporter {
	struct wl_global *global;
	struct wl_listener display_destroy;
};

struct nx_viewport {
	struct wl_resource *resource;
	struct nx_surface *surface;

	struct nx_addon addon;

	struct wl_listener surface_client_commit;
};

static const struct wp_viewport_interface viewport_impl;

// NULL once inert
static struct nx_viewport *viewport_from_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &wp_viewport_interface,
		&viewport_impl));
	return wl_resource_get_user_data(resource);
}

static void viewport_handle_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static bool is_unset_source(double x, double y, double width, double height) {
	return x == -1.0 && y == -1.0 && width == -1.0 && height == -1.0;
}

static void viewport_handle_set_source(struct wl_client *client,
		struct wl_resource *resource, wl_fixed_t x_fixed, wl_fixed_t y_fixed,
		wl_fixed_t width_fixed, wl_fixed_t height_fixed) {
	struct nx_viewport *viewport = viewport_from_resource(resource);
	if (viewport == NULL) {
		wl_resource_post_error(resource, WP_VIEWPORT_ERROR_NO_SURFACE,
			"wp_viewport.set_source sent after wl_surface has been destroyed");
		return;
	}

	double x = wl_fixed_to_double(x_fixed);
	double y = wl_fixed_to_double(y_fixed);
	double width = wl_fixed_to_double(width_fixed);
	double height = wl_fixed_to_double(height_fixed);

	bool unset = is_unset_source(x, y, width, height);
	if (!unset && (x < 0 || y < 0 || width <= 0 || height <= 0)) {
		wl_resource_post_error(resource, WP_VIEWPORT_ERROR_BAD_VALUE,
			"wl_viewport.set_source sent with invalid values");
		return;
	}

	struct nx_surface_state *pending = &viewport->surface->pending;
	pending->viewport.has_src = !unset;
	pending->viewport.src.x = x;
	pending->viewport.src.y = y;
	pending->viewport.src.width = width;
	pending->viewport.src.height = height;

	pending->committed |= NX_SURFACE_STATE_VIEWPORT;
}

static void viewport_handle_set_destination(struct wl_client *client,
		struct wl_resource *resource, int32_t width, int32_t height) {
	struct nx_viewport *viewport = viewport_from_resource(resource);
	if (viewport == NULL) {
		wl_resource_post_error(resource, WP_VIEWPORT_ERROR_NO_SURFACE,
			"wp_viewport.set_destination sent after wl_surface has been destroyed");
		return;
	}

	bool unset = width == -1 && height == -1;
	if (!unset && (width <= 0 || height <= 0)) {
		wl_resource_post_error(resource, WP_VIEWPORT_ERROR_BAD_VALUE,
			"wl_viewport.set_destination sent with invalid values");
		return;
	}

	struct nx_surface_state *pending = &viewport->surface->pending;
	pending->viewport.has_dst = !unset;
	pending->viewport.dst_width = width;
	pending->viewport.dst_height = height;

	pending->committed |= NX_SURFACE_STATE_VIEWPORT;
}

static const struct wp_viewport_interface viewport_impl = {
	.destroy = viewport_handle_destroy,
	.set_source = viewport_handle_set_source,
	.set_destination = viewport_handle_set_destination,
};

static void viewport_destroy(struct nx_viewport *viewport) {
	if (viewport == NULL) {
		return;
	}

	struct nx_surface_state *pending = &viewport->surface->pending;
	pending->viewport.has_src = false;
	pending->viewport.has_dst = false;
	pending->committed |= NX_SURFACE_STATE_VIEWPORT;

	nx_addon_finish(&viewport->addon);

	wl_resource_set_user_data(viewport->resource, NULL);
	wl_list_remove(&viewport->surface_client_commit.link);
	free(viewport);
}

static void surface_addon_destroy(struct nx_addon *addon) {
	struct nx_viewport *viewport = wl_container_of(addon, viewport, addon);
	viewport_destroy(viewport);
}

static const struct nx_addon_interface surface_addon_impl = {
	.name = "nx_viewport",
	.destroy = surface_addon_destroy,
};

static void viewport_handle_resource_destroy(struct wl_resource *resource) {
	viewport_destroy(viewport_from_resource(resource));
}

static bool check_src_buffer_bounds(const struct nx_surface_state *state) {
	int width = state->buffer_width / state->scale;
	int height = state->buffer_height / state->scale;
	nx_output_transform_coords(state->transform, &width, &height);

	struct nx_fbox box = state->viewport.src;
	return box.x + box.width <= width && box.y + box.height <= height;
}

static void viewport_handle_surface_client_commit(struct wl_listener *listener,
		void *data) {
	struct nx_viewport *viewport =
		wl_container_of(listener, viewport, surface_client_commit);

	struct nx_surface_state *state = &viewport->surface->pending;

	if (!state->viewport.has_dst &&
			(floor(state->viewport.src.width) != state->viewport.src.width ||
			floor(state->viewport.src.height) != state->viewport.src.height)) {
		nx_surface_reject_pending(viewport->surface,
			viewport->resource, WP_VIEWPORT_ERROR_BAD_SIZE,
			"wl_viewport.set_source width and height must be integers "
			"when the destination rectangle is unset");
		return;
	}

	if (state->viewport.has_src && nx_surface_state_has_buffer(state) &&
			!check_src_buffer_bounds(state)) {
		nx_surface_reject_pending(viewport->surface,
			viewport->resource, WP_VIEWPORT_ERROR_OUT_OF_BUFFER,
			"source rectangle out of buffer bounds");
	}
}

static void viewporter_handle_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static void viewporter_handle_get_viewport(struct wl_client *client,
		struct wl_resource *resource, uint32_t id,
		struct wl_resource *surface_resource) {
	struct nx_surface *surface = nx_surface_from_resource(surface_resource);

	if (nx_addon_find(&surface->addons, NULL, &surface_addon_impl) != NULL) {
		wl_resource_post_error(resource, WP_VIEWPORTER_ERROR_VIEWPORT_EXISTS,
			"wp_viewport for this surface already exists");
		return;
	}

	struct nx_viewport *viewport = calloc(1, sizeof(*viewport));
	if (viewport == NULL) {
		wl_client_post_no_memory(client);
		return;
	}

	uint32_t version = wl_resource_get_version(resource);
	viewport->resource = wl_resource_create(client, &wp_viewport_interface,
		version, id);
	if (viewport->resource == NULL) {
		wl_client_post_no_memory(client);
		free(viewport);
		return;
	}
	wl_resource_set_implementation(viewport->resource, &viewport_impl,
		viewport, viewport_handle_resource_destroy);

	viewport->surface = surface;

	nx_addon_init(&viewport->addon, &surface->addons, NULL, &surface_addon_impl);

	viewport->surface_client_commit.notify = viewport_handle_surface_client_commit;
	wl_signal_add(&surface->events.client_commit, &viewport->surface_client_commit);
}

static const struct wp_viewporter_interface viewporter_impl = {
	.destroy = viewporter_handle_destroy,
	.get_viewport = viewporter_handle_get_viewport,
};

static void viewporter_bind(struct wl_client *client, void *data,
		uint32_t version, uint32_t id) {
	struct nx_viewporter *viewporter = data;

	struct wl_resource *resource = wl_resource_create(client,
		&wp_viewporter_interface, version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &viewporter_impl, viewporter, NULL);
}

static void handle_display_destroy(struct wl_listener *listener, void *data) {
	struct nx_viewporter *viewporter =
		wl_container_of(listener, viewporter, display_destroy);
	wl_global_destroy(viewporter->global);
	free(viewporter);
}

struct nx_viewporter *nx_viewporter_create(struct wl_display *display) {
	struct nx_viewporter *viewporter = calloc(1, sizeof(*viewporter));
	if (viewporter == NULL) {
		return NULL;
	}

	viewporter->global = wl_global_create(display, &wp_viewporter_interface,
		VIEWPORTER_VERSION, viewporter, viewporter_bind);
	if (viewporter->global == NULL) {
		free(viewporter);
		return NULL;
	}

	viewporter->display_destroy.notify = handle_display_destroy;
	wl_display_add_destroy_listener(display, &viewporter->display_destroy);

	return viewporter;
}
