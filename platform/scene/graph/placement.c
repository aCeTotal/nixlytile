#include <assert.h>
#include "platform/scene/graph/node.h"

// Empty when the node is disabled.
static void scene_node_init_visible(struct nx_scene_node *node,
		pixman_region32_t *visible) {
	pixman_region32_init(visible);
	int x, y;
	if (nx_scene_node_coords(node, &x, &y)) {
		scene_node_visibility(node, visible);
	}
}

void nx_scene_node_set_enabled(struct nx_scene_node *node, bool enabled) {
	if (node->enabled == enabled) {
		return;
	}

	pixman_region32_t visible;
	scene_node_init_visible(node, &visible);

	node->enabled = enabled;

	scene_node_update(node, &visible);
}

void nx_scene_node_set_position(struct nx_scene_node *node, int x, int y) {
	if (node->x == x && node->y == y) {
		return;
	}

	node->x = x;
	node->y = y;
	scene_node_update(node, NULL);
}

void nx_scene_node_place_above(struct nx_scene_node *node,
		struct nx_scene_node *sibling) {
	assert(node != sibling);
	assert(node->parent == sibling->parent);

	if (node->link.prev == &sibling->link) {
		return;
	}

	wl_list_remove(&node->link);
	wl_list_insert(&sibling->link, &node->link);
	scene_node_update(node, NULL);
}

void nx_scene_node_place_below(struct nx_scene_node *node,
		struct nx_scene_node *sibling) {
	assert(node != sibling);
	assert(node->parent == sibling->parent);

	if (node->link.next == &sibling->link) {
		return;
	}

	wl_list_remove(&node->link);
	wl_list_insert(sibling->link.prev, &node->link);
	scene_node_update(node, NULL);
}

void nx_scene_node_raise_to_top(struct nx_scene_node *node) {
	struct nx_scene_node *current_top = wl_container_of(
		node->parent->children.prev, current_top, link);
	if (node == current_top) {
		return;
	}
	nx_scene_node_place_above(node, current_top);
}

void nx_scene_node_lower_to_bottom(struct nx_scene_node *node) {
	struct nx_scene_node *current_bottom = wl_container_of(
		node->parent->children.next, current_bottom, link);
	if (node == current_bottom) {
		return;
	}
	nx_scene_node_place_below(node, current_bottom);
}

void nx_scene_node_reparent(struct nx_scene_node *node,
		struct nx_scene_tree *new_parent) {
	assert(new_parent != NULL);

	if (node->parent == new_parent) {
		return;
	}

	// No node may contain itself.
	for (struct nx_scene_tree *ancestor = new_parent; ancestor != NULL;
			ancestor = ancestor->node.parent) {
		assert(&ancestor->node != node);
	}

	pixman_region32_t visible;
	scene_node_init_visible(node, &visible);

	wl_list_remove(&node->link);
	node->parent = new_parent;
	wl_list_insert(new_parent->children.prev, &node->link);
	scene_node_update(node, &visible);
}
