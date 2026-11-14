#ifndef PLATFORM_SCENE_GRAPH_NODE_H
#define PLATFORM_SCENE_GRAPH_NODE_H

#include "platform/geometry/transform.h"
#include "platform/scene/scene.h"

// One bit each in active_outputs.
#define SCENE_OUTPUT_MAX 64

typedef bool (*scene_node_box_iterator_func_t)(struct nx_scene_node *node,
	int sx, int sy, void *data);

void scene_node_init(struct nx_scene_node *node,
	enum nx_scene_node_type type, struct nx_scene_tree *parent);
struct nx_scene *scene_node_get_root(struct nx_scene_node *node);
bool scene_nodes_in_box(struct nx_scene_node *node, const struct nx_box *box,
	scene_node_box_iterator_func_t iterator, void *user_data);
void scene_node_opaque_region(struct nx_scene_node *node, int x, int y,
	pixman_region32_t *opaque);
void scene_node_visibility(struct nx_scene_node *node,
	pixman_region32_t *visible);
void scene_node_bounds(struct nx_scene_node *node,
	int x, int y, pixman_region32_t *visible);
void scene_update_region(struct nx_scene *scene,
	const pixman_region32_t *update_region);
void scene_node_update(struct nx_scene_node *node,
	pixman_region32_t *damage);
void scene_node_update_outputs(struct nx_scene_node *node,
	struct wl_list *outputs, struct nx_scene_output *ignore,
	struct nx_scene_output *force);
void scene_subtree_update_outputs(struct nx_scene_node *node,
	struct wl_list *outputs, struct nx_scene_output *ignore,
	struct nx_scene_output *force);

static inline float scene_node_inherited_opacity(const struct nx_scene_node *node) {
	float opacity = 1;
	for (const struct nx_scene_tree *tree = node->parent; tree != NULL;
			tree = tree->node.parent) {
		opacity *= tree->opacity;
	}
	return opacity;
}

static inline void scene_buffer_get_size(const struct nx_scene_buffer *scene_buffer,
		int *width, int *height) {
	if (scene_buffer->dst_width > 0 && scene_buffer->dst_height > 0) {
		*width = scene_buffer->dst_width;
		*height = scene_buffer->dst_height;
		return;
	}

	*width = scene_buffer->buffer_width;
	*height = scene_buffer->buffer_height;
	nx_output_transform_coords(scene_buffer->transform, width, height);
}

static inline void scene_node_get_size(struct nx_scene_node *node,
		int *width, int *height) {
	*width = 0;
	*height = 0;

	switch (node->type) {
	case NX_SCENE_NODE_TREE:
		return;
	case NX_SCENE_NODE_RECT:;
		struct nx_scene_rect *scene_rect = nx_scene_rect_from_node(node);
		*width = scene_rect->width;
		*height = scene_rect->height;
		return;
	case NX_SCENE_NODE_BUFFER:
		scene_buffer_get_size(nx_scene_buffer_from_node(node), width, height);
		return;
	}
}

#endif
