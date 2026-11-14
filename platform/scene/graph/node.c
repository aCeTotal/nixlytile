#include <assert.h>
#include <stdlib.h>
#include "platform/render/drm_syncobj.h"
#include "platform/scene/graph/buffer.h"
#include "platform/scene/graph/node.h"

struct nx_scene *scene_node_get_root(struct nx_scene_node *node) {
	struct nx_scene_tree *tree = node->type == NX_SCENE_NODE_TREE ?
		nx_scene_tree_from_node(node) : node->parent;
	while (tree->node.parent != NULL) {
		tree = tree->node.parent;
	}
	struct nx_scene *scene = wl_container_of(tree, scene, tree);
	return scene;
}

void scene_node_init(struct nx_scene_node *node,
		enum nx_scene_node_type type, struct nx_scene_tree *parent) {
	*node = (struct nx_scene_node){
		.type = type,
		.parent = parent,
		.enabled = true,
	};

	wl_list_init(&node->link);

	wl_signal_init(&node->events.destroy);
	pixman_region32_init(&node->visible);

	if (parent != NULL) {
		wl_list_insert(parent->children.prev, &node->link);
	}

	nx_addon_set_init(&node->addons);
}

static void scene_buffer_finish(struct nx_scene_buffer *scene_buffer) {
	scene_buffer_set_buffer(scene_buffer, NULL);
	scene_buffer_set_texture(scene_buffer, NULL);
	pixman_region32_fini(&scene_buffer->opaque_region);
	nx_drm_syncobj_timeline_unref(scene_buffer->wait_timeline);

	assert(wl_list_empty(&scene_buffer->events.outputs_update.listener_list));
	assert(wl_list_empty(&scene_buffer->events.output_sample.listener_list));
	assert(wl_list_empty(&scene_buffer->events.frame_done.listener_list));
}

static void scene_root_finish(struct nx_scene *scene) {
	struct nx_scene_output *scene_output, *scene_output_tmp;
	wl_list_for_each_safe(scene_output, scene_output_tmp, &scene->outputs, link) {
		nx_scene_output_destroy(scene_output);
	}

	wl_list_remove(&scene->linux_dmabuf_v1_destroy.link);
	wl_list_remove(&scene->gamma_control_manager_v1_destroy.link);
	wl_list_remove(&scene->gamma_control_manager_v1_set_gamma.link);
}

static void scene_tree_finish(struct nx_scene_tree *scene_tree) {
	struct nx_scene *scene = scene_node_get_root(&scene_tree->node);
	bool is_root = scene_tree == &scene->tree;
	assert(is_root == (scene_tree->node.parent == NULL));
	if (is_root) {
		scene_root_finish(scene);
	}

	struct nx_scene_node *child, *child_tmp;
	wl_list_for_each_safe(child, child_tmp, &scene_tree->children, link) {
		nx_scene_node_destroy(child);
	}
}

void nx_scene_node_destroy(struct nx_scene_node *node) {
	if (node == NULL) {
		return;
	}

	// Listeners may remove children first.
	wl_signal_emit_mutable(&node->events.destroy, NULL);
	nx_addon_set_finish(&node->addons);

	nx_scene_node_set_enabled(node, false);

	switch (node->type) {
	case NX_SCENE_NODE_TREE:
		scene_tree_finish(nx_scene_tree_from_node(node));
		break;
	case NX_SCENE_NODE_RECT:
		break;
	case NX_SCENE_NODE_BUFFER:
		scene_buffer_finish(nx_scene_buffer_from_node(node));
		break;
	}

	assert(wl_list_empty(&node->events.destroy.listener_list));

	wl_list_remove(&node->link);
	pixman_region32_fini(&node->visible);
	free(node);
}

bool nx_scene_node_coords(struct nx_scene_node *node,
		int *lx_ptr, int *ly_ptr) {
	assert(node);

	int lx = 0, ly = 0;
	bool enabled = true;
	while (true) {
		lx += node->x;
		ly += node->y;
		enabled = enabled && node->enabled;
		if (node->parent == NULL) {
			break;
		}

		node = &node->parent->node;
	}

	*lx_ptr = lx;
	*ly_ptr = ly;
	return enabled;
}
