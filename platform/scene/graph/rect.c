#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "platform/scene/graph/node.h"

struct nx_scene_rect *nx_scene_rect_create(struct nx_scene_tree *parent,
		int width, int height, const float color[static 4]) {
	assert(parent);
	assert(width >= 0 && height >= 0);

	struct nx_scene_rect *scene_rect = calloc(1, sizeof(*scene_rect));
	if (scene_rect == NULL) {
		return NULL;
	}
	scene_node_init(&scene_rect->node, NX_SCENE_NODE_RECT, parent);

	scene_rect->width = width;
	scene_rect->height = height;
	memcpy(scene_rect->color, color, sizeof(scene_rect->color));

	scene_node_update(&scene_rect->node, NULL);

	return scene_rect;
}

void nx_scene_rect_set_size(struct nx_scene_rect *rect, int width, int height) {
	if (rect->width == width && rect->height == height) {
		return;
	}

	assert(width >= 0 && height >= 0);

	rect->width = width;
	rect->height = height;
	scene_node_update(&rect->node, NULL);
}

void nx_scene_rect_set_color(struct nx_scene_rect *rect, const float color[static 4]) {
	if (memcmp(rect->color, color, sizeof(rect->color)) == 0) {
		return;
	}

	memcpy(rect->color, color, sizeof(rect->color));
	scene_node_update(&rect->node, NULL);
}

void nx_scene_rect_set_shape(struct nx_scene_rect *rect,
		const struct nx_scene_rect_shape *shape) {
	if (memcmp(&rect->shape, shape, sizeof(rect->shape)) == 0) {
		return;
	}

	assert(shape->radius >= 0 && shape->softness >= 0 && shape->border >= 0);
	rect->shape = *shape;
	scene_node_update(&rect->node, NULL);
}
