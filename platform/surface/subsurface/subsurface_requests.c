#include <assert.h>
#include <inttypes.h>
#include <wayland-server-protocol.h>
#include "platform/surface/subsurface/subsurface.h"

struct nx_subsurface *subsurface_from_resource(struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &wl_subsurface_interface,
		&subsurface_implementation));
	return wl_resource_get_user_data(resource);
}

static void subsurface_handle_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static void subsurface_handle_set_position(struct wl_client *client,
		struct wl_resource *resource, int32_t x, int32_t y) {
	struct nx_subsurface *subsurface = subsurface_from_resource(resource);
	if (subsurface == NULL) {
		return;
	}

	subsurface->pending.x = x;
	subsurface->pending.y = y;
}

static struct nx_subsurface *subsurface_find_sibling(
		struct nx_subsurface *subsurface, struct nx_surface *surface) {
	struct nx_surface *parent = subsurface->parent;

	struct nx_subsurface *sibling;
	wl_list_for_each(sibling, &parent->pending.subsurfaces_below, pending.link) {
		if (sibling->surface == surface && sibling != subsurface) {
			return sibling;
		}
	}
	wl_list_for_each(sibling, &parent->pending.subsurfaces_above, pending.link) {
		if (sibling->surface == surface && sibling != subsurface) {
			return sibling;
		}
	}

	return NULL;
}

// Insertion point, NULL if invalid
static struct wl_list *placement_anchor(struct nx_subsurface *subsurface,
		struct wl_resource *sibling_resource, struct wl_list *parent_list) {
	struct nx_surface *sibling_surface = nx_surface_from_resource(sibling_resource);
	if (sibling_surface == subsurface->parent) {
		return parent_list;
	}
	struct nx_subsurface *sibling = subsurface_find_sibling(subsurface, sibling_surface);
	if (sibling == NULL) {
		return NULL;
	}
	return &sibling->pending.link;
}

static void subsurface_handle_place_above(struct wl_client *client,
		struct wl_resource *resource, struct wl_resource *sibling_resource) {
	struct nx_subsurface *subsurface = subsurface_from_resource(resource);
	if (subsurface == NULL) {
		return;
	}

	struct wl_list *node = placement_anchor(subsurface, sibling_resource,
		&subsurface->parent->pending.subsurfaces_above);
	if (node == NULL) {
		wl_resource_post_error(subsurface->resource,
			WL_SUBSURFACE_ERROR_BAD_SURFACE,
			"%s: wl_surface@%" PRIu32 "is not a parent or sibling",
			"place_above", wl_resource_get_id(sibling_resource));
		return;
	}

	wl_list_remove(&subsurface->pending.link);
	wl_list_insert(node, &subsurface->pending.link);
}

static void subsurface_handle_place_below(struct wl_client *client,
		struct wl_resource *resource, struct wl_resource *sibling_resource) {
	struct nx_subsurface *subsurface = subsurface_from_resource(resource);
	if (subsurface == NULL) {
		return;
	}

	struct wl_list *node = placement_anchor(subsurface, sibling_resource,
		&subsurface->parent->pending.subsurfaces_below);
	if (node == NULL) {
		wl_resource_post_error(subsurface->resource,
			WL_SUBSURFACE_ERROR_BAD_SURFACE,
			"%s: wl_surface@%" PRIu32 " is not a parent or sibling",
			"place_below", wl_resource_get_id(sibling_resource));
		return;
	}

	wl_list_remove(&subsurface->pending.link);
	wl_list_insert(node->prev, &subsurface->pending.link);
}

static void subsurface_handle_set_sync(struct wl_client *client,
		struct wl_resource *resource) {
	struct nx_subsurface *subsurface = subsurface_from_resource(resource);
	if (subsurface == NULL) {
		return;
	}

	subsurface->synchronized = true;
}

static void subsurface_handle_set_desync(struct wl_client *client,
		struct wl_resource *resource) {
	struct nx_subsurface *subsurface = subsurface_from_resource(resource);
	if (subsurface == NULL || !subsurface->synchronized) {
		return;
	}

	subsurface->synchronized = false;

	if (subsurface_is_synchronized(subsurface) || !subsurface->has_cache) {
		return;
	}
	nx_surface_unlock_cached(subsurface->surface, subsurface->cached_seq);
	subsurface->has_cache = false;
}

const struct wl_subsurface_interface subsurface_implementation = {
	.destroy = subsurface_handle_destroy,
	.set_position = subsurface_handle_set_position,
	.place_above = subsurface_handle_place_above,
	.place_below = subsurface_handle_place_below,
	.set_sync = subsurface_handle_set_sync,
	.set_desync = subsurface_handle_set_desync,
};
