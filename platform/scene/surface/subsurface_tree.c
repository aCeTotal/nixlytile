#include <assert.h>
#include <stdlib.h>
#include "platform/surface/compositor.h"
#include "platform/surface/subcompositor.h"
#include "platform/scene/surface/subsurface_tree.h"

static void subsurface_tree_addon_destroy(struct nx_addon *addon) {
	struct scene_subsurface_tree *subsurface_tree =
		wl_container_of(addon, subsurface_tree, scene_addon);
	// The tree node owns the rest.
	if (subsurface_tree->parent) {
		nx_addon_finish(&subsurface_tree->surface_addon);
		wl_list_remove(&subsurface_tree->subsurface_destroy.link);
	}
	nx_addon_finish(&subsurface_tree->scene_addon);
	wl_list_remove(&subsurface_tree->surface_destroy.link);
	wl_list_remove(&subsurface_tree->surface_commit.link);
	wl_list_remove(&subsurface_tree->surface_map.link);
	wl_list_remove(&subsurface_tree->surface_unmap.link);
	wl_list_remove(&subsurface_tree->surface_new_subsurface.link);
	free(subsurface_tree);
}

static void subsurface_tree_surface_addon_destroy(struct nx_addon *addon) {
	struct scene_subsurface_tree *subsurface_tree =
		wl_container_of(addon, subsurface_tree, surface_addon);
	nx_scene_node_destroy(&subsurface_tree->tree->node);
}

static const struct nx_addon_interface subsurface_tree_surface_addon_impl = {
	.name = "nx_scene_subsurface_tree",
	.destroy = subsurface_tree_surface_addon_destroy,
};

struct scene_subsurface_tree *scene_subsurface_tree_from_subsurface(
		struct scene_subsurface_tree *parent,
		struct nx_subsurface *subsurface) {
	struct nx_addon *addon = nx_addon_find(&subsurface->surface->addons,
		parent, &subsurface_tree_surface_addon_impl);
	assert(addon != NULL);
	struct scene_subsurface_tree *subsurface_tree =
		wl_container_of(addon, subsurface_tree, surface_addon);
	return subsurface_tree;
}

static void subsurface_tree_handle_surface_destroy(struct wl_listener *listener,
		void *data) {
	struct scene_subsurface_tree *subsurface_tree =
		wl_container_of(listener, subsurface_tree, surface_destroy);
	nx_scene_node_destroy(&subsurface_tree->tree->node);
}

static void subsurface_tree_handle_surface_commit(struct wl_listener *listener,
		void *data) {
	struct scene_subsurface_tree *subsurface_tree =
		wl_container_of(listener, subsurface_tree, surface_commit);

	scene_subsurface_tree_reconfigure(subsurface_tree);
}

static void subsurface_tree_handle_subsurface_destroy(struct wl_listener *listener,
		void *data) {
	struct scene_subsurface_tree *subsurface_tree =
		wl_container_of(listener, subsurface_tree, subsurface_destroy);
	nx_scene_node_destroy(&subsurface_tree->tree->node);
}

static void subsurface_tree_handle_surface_map(struct wl_listener *listener,
		void *data) {
	struct scene_subsurface_tree *subsurface_tree =
		wl_container_of(listener, subsurface_tree, surface_map);

	nx_scene_node_set_enabled(&subsurface_tree->tree->node, true);
}

static void subsurface_tree_handle_surface_unmap(struct wl_listener *listener,
		void *data) {
	struct scene_subsurface_tree *subsurface_tree =
		wl_container_of(listener, subsurface_tree, surface_unmap);

	nx_scene_node_set_enabled(&subsurface_tree->tree->node, false);
}

static struct scene_subsurface_tree *scene_surface_tree_create(
	struct nx_scene_tree *parent, struct nx_surface *surface);

static bool subsurface_tree_create_subsurface(
		struct scene_subsurface_tree *parent,
		struct nx_subsurface *subsurface) {
	struct scene_subsurface_tree *child = scene_surface_tree_create(
		parent->tree, subsurface->surface);
	if (child == NULL) {
		return false;
	}

	child->parent = parent;

	nx_addon_init(&child->surface_addon, &subsurface->surface->addons,
		parent, &subsurface_tree_surface_addon_impl);

	child->subsurface_destroy.notify = subsurface_tree_handle_subsurface_destroy;
	wl_signal_add(&subsurface->events.destroy, &child->subsurface_destroy);

	return true;
}

static void subsurface_tree_handle_surface_new_subsurface(
		struct wl_listener *listener, void *data) {
	struct scene_subsurface_tree *subsurface_tree =
		wl_container_of(listener, subsurface_tree, surface_new_subsurface);
	struct nx_subsurface *subsurface = data;
	if (!subsurface_tree_create_subsurface(subsurface_tree, subsurface)) {
		wl_resource_post_no_memory(subsurface->resource);
	}
}

static const struct nx_addon_interface subsurface_tree_addon_impl = {
	.name = "nx_scene_subsurface_tree",
	.destroy = subsurface_tree_addon_destroy,
};

static void subsurface_tree_add_listeners(struct scene_subsurface_tree *subsurface_tree) {
	struct nx_surface *surface = subsurface_tree->surface;

	subsurface_tree->surface_destroy.notify = subsurface_tree_handle_surface_destroy;
	wl_signal_add(&surface->events.destroy, &subsurface_tree->surface_destroy);

	subsurface_tree->surface_commit.notify = subsurface_tree_handle_surface_commit;
	wl_signal_add(&surface->events.commit, &subsurface_tree->surface_commit);

	subsurface_tree->surface_map.notify = subsurface_tree_handle_surface_map;
	wl_signal_add(&surface->events.map, &subsurface_tree->surface_map);

	subsurface_tree->surface_unmap.notify = subsurface_tree_handle_surface_unmap;
	wl_signal_add(&surface->events.unmap, &subsurface_tree->surface_unmap);

	subsurface_tree->surface_new_subsurface.notify =
		subsurface_tree_handle_surface_new_subsurface;
	wl_signal_add(&surface->events.new_subsurface,
		&subsurface_tree->surface_new_subsurface);
}

static bool subsurface_tree_create_children(
		struct scene_subsurface_tree *subsurface_tree, struct wl_list *subsurfaces) {
	struct nx_subsurface *subsurface;
	wl_list_for_each(subsurface, subsurfaces, current.link) {
		if (!subsurface_tree_create_subsurface(subsurface_tree, subsurface)) {
			return false;
		}
	}
	return true;
}

static struct scene_subsurface_tree *scene_surface_tree_create(
		struct nx_scene_tree *parent, struct nx_surface *surface) {
	struct scene_subsurface_tree *subsurface_tree =
		calloc(1, sizeof(*subsurface_tree));
	if (subsurface_tree == NULL) {
		return NULL;
	}

	subsurface_tree->tree = nx_scene_tree_create(parent);
	if (subsurface_tree->tree == NULL) {
		goto error_surface_tree;
	}

	subsurface_tree->scene_surface =
		nx_scene_surface_create(subsurface_tree->tree, surface);
	if (subsurface_tree->scene_surface == NULL) {
		goto error_scene_surface;
	}

	subsurface_tree->surface = surface;

	if (!subsurface_tree_create_children(subsurface_tree, &surface->current.subsurfaces_below) ||
			!subsurface_tree_create_children(subsurface_tree, &surface->current.subsurfaces_above)) {
		goto error_scene_surface;
	}

	scene_subsurface_tree_reconfigure(subsurface_tree);

	nx_addon_init(&subsurface_tree->scene_addon, &subsurface_tree->tree->node.addons,
		NULL, &subsurface_tree_addon_impl);

	subsurface_tree_add_listeners(subsurface_tree);

	nx_scene_node_set_enabled(&subsurface_tree->tree->node, surface->mapped);

	return subsurface_tree;

error_scene_surface:
	nx_scene_node_destroy(&subsurface_tree->tree->node);
error_surface_tree:
	free(subsurface_tree);
	return NULL;
}

struct nx_scene_tree *nx_scene_subsurface_tree_create(
		struct nx_scene_tree *parent, struct nx_surface *surface) {
	struct scene_subsurface_tree *subsurface_tree =
		scene_surface_tree_create(parent, surface);
	if (subsurface_tree == NULL) {
		return NULL;
	}
	return subsurface_tree->tree;
}

struct scene_subsurface_tree *scene_subsurface_tree_from_node(
		struct nx_scene_node *node) {
	struct nx_addon *addon = nx_addon_find(&node->addons, NULL, &subsurface_tree_addon_impl);
	if (!addon) {
		return NULL;
	}

	struct scene_subsurface_tree *tree =
		wl_container_of(addon, tree, scene_addon);
	return tree;
}
