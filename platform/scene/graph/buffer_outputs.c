#include <assert.h>
#include "platform/output/output.h"
#include "platform/scene/graph/node.h"

struct output_match {
	uint64_t active;
	size_t count;
	struct nx_scene_output *primary;
};

static uint32_t region_area(const pixman_region32_t *region) {
	uint32_t area = 0;

	int nrects;
	pixman_box32_t *rects = pixman_region32_rectangles(region, &nrects);
	for (int i = 0; i < nrects; ++i) {
		area += (rects[i].x2 - rects[i].x1) * (rects[i].y2 - rects[i].y1);
	}

	return area;
}

static uint32_t visible_area_on_outputs(struct nx_scene_node *node,
		struct wl_list *outputs) {
	pixman_region32_t visible;
	pixman_region32_init(&visible);
	struct nx_scene_output *scene_output;
	wl_list_for_each(scene_output, outputs, link) {
		int width, height;
		nx_output_effective_resolution(scene_output->output, &width, &height);
		pixman_region32_union_rect(&visible, &visible,
			scene_output->x, scene_output->y, width, height);
	}
	pixman_region32_intersect(&visible, &visible, &node->visible);
	uint32_t area = region_area(&visible);
	pixman_region32_fini(&visible);
	return area;
}

static uint32_t output_overlap(struct nx_scene_node *node,
		struct nx_scene_output *scene_output) {
	struct nx_box output_box = {
		.x = scene_output->x,
		.y = scene_output->y,
	};
	nx_output_effective_resolution(scene_output->output,
		&output_box.width, &output_box.height);

	pixman_region32_t intersection;
	pixman_region32_init(&intersection);
	pixman_region32_intersect_rect(&intersection, &node->visible,
		output_box.x, output_box.y, output_box.width, output_box.height);
	uint32_t overlap = region_area(&intersection);
	pixman_region32_fini(&intersection);
	return overlap;
}

static struct output_match match_outputs(struct nx_scene_node *node,
		struct wl_list *outputs, struct nx_scene_output *ignore) {
	struct output_match match = {0};
	if (pixman_region32_empty(&node->visible)) {
		return match;
	}

	uint32_t visible_area = visible_area_on_outputs(node, outputs);
	uint32_t largest_overlap = 0;
	struct nx_scene_output *scene_output;
	wl_list_for_each(scene_output, outputs, link) {
		if (scene_output == ignore || !scene_output->output->enabled) {
			continue;
		}

		// Ignore outputs showing 10% or less.
		uint32_t overlap = output_overlap(node, scene_output);
		if (overlap <= 0.1 * visible_area) {
			continue;
		}

		if (overlap >= largest_overlap) {
			largest_overlap = overlap;
			match.primary = scene_output;
		}
		match.active |= 1ull << scene_output->index;
		match.count++;
	}
	return match;
}

static void scene_buffer_emit_outputs_update(struct nx_scene_buffer *scene_buffer,
		struct wl_list *outputs, const struct output_match *match) {
	struct nx_scene_output *outputs_array[SCENE_OUTPUT_MAX];
	struct nx_scene_outputs_update_event event = {
		.active = outputs_array,
		.size = match->count,
	};

	size_t i = 0;
	struct nx_scene_output *scene_output;
	wl_list_for_each(scene_output, outputs, link) {
		if (~match->active & (1ull << scene_output->index)) {
			continue;
		}

		assert(i < match->count);
		outputs_array[i++] = scene_output;
	}

	wl_signal_emit_mutable(&scene_buffer->events.outputs_update, &event);
}

void scene_node_update_outputs(struct nx_scene_node *node,
		struct wl_list *outputs, struct nx_scene_output *ignore,
		struct nx_scene_output *force) {
	if (node->type != NX_SCENE_NODE_BUFFER) {
		return;
	}

	struct nx_scene_buffer *scene_buffer = nx_scene_buffer_from_node(node);
	struct nx_scene_output *old_primary_output = scene_buffer->primary_output;
	uint64_t old_active = scene_buffer->active_outputs;

	struct output_match match = match_outputs(node, outputs, ignore);
	scene_buffer->primary_output = match.primary;
	scene_buffer->active_outputs = match.active;

	// Active outputs imply a primary one.
	assert(!scene_buffer->active_outputs || scene_buffer->primary_output);

	bool forced = force && ((1ull << force->index) & match.active);
	if (old_active == match.active && !forced &&
			old_primary_output == match.primary) {
		return;
	}

	scene_buffer_emit_outputs_update(scene_buffer, outputs, &match);
}

void scene_subtree_update_outputs(struct nx_scene_node *node,
		struct wl_list *outputs, struct nx_scene_output *ignore,
		struct nx_scene_output *force) {
	if (node->type != NX_SCENE_NODE_TREE) {
		scene_node_update_outputs(node, outputs, ignore, force);
		return;
	}

	struct nx_scene_tree *scene_tree = nx_scene_tree_from_node(node);
	struct nx_scene_node *child;
	wl_list_for_each(child, &scene_tree->children, link) {
		scene_subtree_update_outputs(child, outputs, ignore, force);
	}
}
