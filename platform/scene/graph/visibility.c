#include "platform/scene/graph/node.h"

// Callers check tree opacity first.
void scene_node_opaque_region(struct nx_scene_node *node, int x, int y,
		pixman_region32_t *opaque) {
	int width, height;
	scene_node_get_size(node, &width, &height);

	if (node->type == NX_SCENE_NODE_RECT &&
			nx_scene_rect_from_node(node)->color[3] != 1) {
		return;
	}

	struct nx_scene_buffer *scene_buffer = node->type == NX_SCENE_NODE_BUFFER ?
		nx_scene_buffer_from_node(node) : NULL;
	if (scene_buffer != NULL &&
			(!scene_buffer->buffer || scene_buffer->opacity != 1)) {
		return;
	}
	if (scene_buffer != NULL && !scene_buffer->buffer_is_opaque) {
		pixman_region32_copy(opaque, &scene_buffer->opaque_region);
		pixman_region32_intersect_rect(opaque, opaque, 0, 0, width, height);
		pixman_region32_translate(opaque, x, y);
		return;
	}

	pixman_region32_fini(opaque);
	pixman_region32_init_rect(opaque, x, y, width, height);
}

void scene_node_visibility(struct nx_scene_node *node,
		pixman_region32_t *visible) {
	if (!node->enabled) {
		return;
	}

	if (node->type == NX_SCENE_NODE_TREE) {
		struct nx_scene_tree *scene_tree = nx_scene_tree_from_node(node);
		struct nx_scene_node *child;
		wl_list_for_each(child, &scene_tree->children, link) {
			scene_node_visibility(child, visible);
		}
		return;
	}

	pixman_region32_union(visible, visible, &node->visible);
}

void scene_node_bounds(struct nx_scene_node *node,
		int x, int y, pixman_region32_t *visible) {
	if (!node->enabled) {
		return;
	}

	if (node->type == NX_SCENE_NODE_TREE) {
		struct nx_scene_tree *scene_tree = nx_scene_tree_from_node(node);
		struct nx_scene_node *child;
		wl_list_for_each(child, &scene_tree->children, link) {
			scene_node_bounds(child, x + child->x, y + child->y, visible);
		}
		return;
	}

	int width, height;
	scene_node_get_size(node, &width, &height);
	pixman_region32_union_rect(visible, visible, x, y, width, height);
}
