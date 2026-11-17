#include "platform/scene/graph/node.h"

// Clips to box minus corners.
static void crop_corners(pixman_region32_t *region, const struct nx_box *box, int radius) {
	int half = (box->width < box->height ? box->width : box->height) / 2;
	int r = radius < half ? radius : half;

	pixman_region32_t cross;
	pixman_region32_init_rect(&cross, box->x + r, box->y, box->width - 2 * r, box->height);
	pixman_region32_union_rect(&cross, &cross, box->x, box->y + r,
		box->width, box->height - 2 * r);
	pixman_region32_intersect(region, region, &cross);
	pixman_region32_fini(&cross);
}

static void rect_opaque_region(const struct nx_scene_rect *scene_rect,
		pixman_region32_t *opaque) {
	const struct nx_scene_rect_shape *shape = &scene_rect->shape;
	if (scene_rect->color[3] != 1 || shape->softness > 0 || shape->border > 0) {
		return;
	}

	struct nx_box box = { .width = scene_rect->width, .height = scene_rect->height };
	pixman_region32_union_rect(opaque, opaque, 0, 0, box.width, box.height);
	if (shape->radius > 0) {
		crop_corners(opaque, &box, shape->radius);
	}
}

static void buffer_opaque_region(const struct nx_scene_buffer *scene_buffer,
		pixman_region32_t *opaque) {
	if (!scene_buffer->buffer || scene_buffer->opacity != 1) {
		return;
	}

	int width, height;
	scene_buffer_get_size(scene_buffer, &width, &height);
	if (scene_buffer->buffer_is_opaque) {
		pixman_region32_union_rect(opaque, opaque, 0, 0, width, height);
	} else {
		pixman_region32_intersect_rect(opaque, &scene_buffer->opaque_region,
			0, 0, width, height);
	}
	if (scene_buffer->corner_radius > 0) {
		crop_corners(opaque, &scene_buffer->corner_box, scene_buffer->corner_radius);
	}
}

// Callers check tree opacity first.
void scene_node_opaque_region(struct nx_scene_node *node, int x, int y,
		pixman_region32_t *opaque) {
	switch (node->type) {
	case NX_SCENE_NODE_TREE:
		return;
	case NX_SCENE_NODE_RECT:
		rect_opaque_region(nx_scene_rect_from_node(node), opaque);
		break;
	case NX_SCENE_NODE_BUFFER:
		buffer_opaque_region(nx_scene_buffer_from_node(node), opaque);
		break;
	}
	pixman_region32_translate(opaque, x, y);
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
