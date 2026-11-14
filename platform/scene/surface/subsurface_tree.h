#ifndef PLATFORM_SCENE_SURFACE_SUBSURFACE_TREE_H
#define PLATFORM_SCENE_SURFACE_SUBSURFACE_TREE_H

#include "platform/scene/scene.h"

// Surface node plus sub-surface subtrees.
struct scene_subsurface_tree {
	struct nx_scene_tree *tree;
	struct nx_surface *surface;
	struct nx_scene_surface *scene_surface;

	struct wl_listener surface_destroy;
	struct wl_listener surface_commit;
	struct wl_listener surface_map;
	struct wl_listener surface_unmap;
	struct wl_listener surface_new_subsurface;

	struct scene_subsurface_tree *parent; // NULL for the top-level surface

	struct nx_addon scene_addon;

	struct nx_box clip;

	// sub-surfaces only
	struct nx_addon surface_addon;
	struct wl_listener subsurface_destroy;
};

struct scene_subsurface_tree *scene_subsurface_tree_from_subsurface(
	struct scene_subsurface_tree *parent, struct nx_subsurface *subsurface);
struct scene_subsurface_tree *scene_subsurface_tree_from_node(
	struct nx_scene_node *node);
void scene_subsurface_tree_reconfigure(
	struct scene_subsurface_tree *subsurface_tree);

#endif
