#include "platform/scene/scene.h"

void nx_scene_buffer_send_frame_done(struct nx_scene_buffer *scene_buffer,
		struct nx_scene_frame_done_event *event) {
	if (pixman_region32_empty(&scene_buffer->node.visible)) {
		return;
	}

	wl_signal_emit_mutable(&scene_buffer->events.frame_done, event);
}

static void scene_node_send_frame_done(struct nx_scene_node *node,
		struct nx_scene_frame_done_event *event) {
	if (!node->enabled) {
		return;
	}

	if (node->type == NX_SCENE_NODE_BUFFER) {
		nx_scene_buffer_send_frame_done(nx_scene_buffer_from_node(node), event);
		return;
	}
	if (node->type != NX_SCENE_NODE_TREE) {
		return;
	}

	struct nx_scene_tree *scene_tree = nx_scene_tree_from_node(node);
	struct nx_scene_node *child;
	wl_list_for_each(child, &scene_tree->children, link) {
		scene_node_send_frame_done(child, event);
	}
}

void nx_scene_output_send_frame_done(struct nx_scene_output *scene_output,
		struct timespec *now) {
	struct nx_scene_frame_done_event event = {
		.output = scene_output,
		.when = *now,
	};
	scene_node_send_frame_done(&scene_output->scene->tree.node, &event);
}
