#include <assert.h>
#include "platform/surface/compositor.h"
#include "platform/surface/subcompositor.h"
#include "platform/scene/surface/scene_surface.h"
#include "platform/scene/surface/subsurface_tree.h"

// Returns whether a clip applies.
static bool subsurface_tree_reconfigure_clip(
		struct scene_subsurface_tree *subsurface_tree) {
	struct nx_scene_node *surface_node = &subsurface_tree->scene_surface->buffer->node;
	if (subsurface_tree->parent) {
		subsurface_tree->clip = (struct nx_box){
			.x = subsurface_tree->parent->clip.x - subsurface_tree->tree->node.x,
			.y = subsurface_tree->parent->clip.y - subsurface_tree->tree->node.y,
			.width = subsurface_tree->parent->clip.width,
			.height = subsurface_tree->parent->clip.height,
		};
	}

	if (nx_box_empty(&subsurface_tree->clip)) {
		scene_surface_set_clip(subsurface_tree->scene_surface, NULL);
		nx_scene_node_set_enabled(surface_node, true);
		nx_scene_node_set_position(surface_node, 0, 0);
		return false;
	}

	struct nx_box clip = subsurface_tree->clip;
	struct nx_box surface_box = {
		.width = subsurface_tree->surface->current.width,
		.height = subsurface_tree->surface->current.height,
	};

	bool intersects = nx_box_intersection(&clip, &clip, &surface_box);
	nx_scene_node_set_enabled(surface_node, intersects);
	if (intersects) {
		nx_scene_node_set_position(surface_node, clip.x, clip.y);
		scene_surface_set_clip(subsurface_tree->scene_surface, &clip);
	}

	return true;
}

void scene_subsurface_tree_reconfigure(
		struct scene_subsurface_tree *subsurface_tree) {
	bool has_clip = subsurface_tree_reconfigure_clip(subsurface_tree);

	struct nx_surface *surface = subsurface_tree->surface;

	struct nx_scene_node *prev = NULL;
	struct nx_subsurface *subsurface;
	wl_list_for_each(subsurface, &surface->current.subsurfaces_below,
			current.link) {
		struct scene_subsurface_tree *child =
			scene_subsurface_tree_from_subsurface(subsurface_tree, subsurface);
		if (prev != NULL) {
			nx_scene_node_place_above(&child->tree->node, prev);
		}
		prev = &child->tree->node;

		nx_scene_node_set_position(&child->tree->node,
			subsurface->current.x, subsurface->current.y);

		if (has_clip) {
			subsurface_tree_reconfigure_clip(child);
		}
	}

	if (prev != NULL) {
		nx_scene_node_place_above(&subsurface_tree->scene_surface->buffer->node, prev);
	}
	prev = &subsurface_tree->scene_surface->buffer->node;

	wl_list_for_each(subsurface, &surface->current.subsurfaces_above,
			current.link) {
		struct scene_subsurface_tree *child =
			scene_subsurface_tree_from_subsurface(subsurface_tree, subsurface);
		nx_scene_node_place_above(&child->tree->node, prev);
		prev = &child->tree->node;

		nx_scene_node_set_position(&child->tree->node,
			subsurface->current.x, subsurface->current.y);

		if (has_clip) {
			subsurface_tree_reconfigure_clip(child);
		}
	}
}

// False for an unchanged root clip.
static bool subsurface_tree_apply_clip(struct scene_subsurface_tree *tree,
		const struct nx_box *clip) {
	bool is_root = tree->parent == NULL;
	if (is_root && nx_box_equal(&tree->clip, clip)) {
		return false;
	}
	if (is_root) {
		tree->clip = clip ? *clip : (struct nx_box){0};
	}

	subsurface_tree_reconfigure_clip(tree);
	return true;
}

static bool subsurface_tree_set_clip(struct nx_scene_node *node,
		const struct nx_box *clip) {
	if (node->type != NX_SCENE_NODE_TREE) {
		return false;
	}

	struct scene_subsurface_tree *tree = scene_subsurface_tree_from_node(node);
	if (tree && !subsurface_tree_apply_clip(tree, clip)) {
		return true;
	}

	bool discovered_subsurface_tree = tree != NULL;
	struct nx_scene_tree *scene_tree = nx_scene_tree_from_node(node);
	struct nx_scene_node *child;
	wl_list_for_each(child, &scene_tree->children, link) {
		discovered_subsurface_tree |= subsurface_tree_set_clip(child, clip);
	}

	return discovered_subsurface_tree;
}

void nx_scene_subsurface_tree_set_clip(struct nx_scene_node *node,
		const struct nx_box *clip) {
	bool found = subsurface_tree_set_clip(node, clip);
	assert(found);
}
