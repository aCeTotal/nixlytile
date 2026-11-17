#include <math.h>
#include <string.h>
#include "platform/container/array.h"
#include "platform/scene/graph/node.h"
#include "platform/scene/render/frame.h"

struct render_list_constructor_data {
	struct nx_box box;
	struct wl_array *render_list;
	bool calculate_visibility;
	bool fractional_scale;
};

static bool scene_node_invisible(struct nx_scene_node *node) {
	switch (node->type) {
	case NX_SCENE_NODE_TREE:
		return true;
	case NX_SCENE_NODE_RECT:
		return nx_scene_rect_from_node(node)->color[3] == 0.f;
	case NX_SCENE_NODE_BUFFER:;
		struct nx_scene_buffer *buffer = nx_scene_buffer_from_node(node);
		return buffer->buffer == NULL && buffer->texture == NULL;
	}

	return false;
}

static bool scene_buffer_is_black_opaque(struct nx_scene_buffer *scene_buffer) {
	return scene_buffer->is_single_pixel_buffer &&
		scene_buffer->single_pixel_buffer_color[0] == 0 &&
		scene_buffer->single_pixel_buffer_color[1] == 0 &&
		scene_buffer->single_pixel_buffer_color[2] == 0 &&
		scene_buffer->single_pixel_buffer_color[3] == UINT32_MAX &&
		scene_buffer->opacity == 1.0 &&
		scene_buffer->corner_radius == 0;
}

static bool scene_rect_is_black_opaque(struct nx_scene_rect *scene_rect) {
	static const float black[4] = { 0.f, 0.f, 0.f, 1.f };
	const struct nx_scene_rect_shape *shape = &scene_rect->shape;
	return memcmp(scene_rect->color, black, sizeof(black)) == 0 &&
		shape->radius == 0 && shape->softness == 0 && shape->border == 0;
}

static bool scene_node_is_opaque_black(struct nx_scene_node *node, float opacity) {
	if (opacity != 1) {
		return false;
	}

	switch (node->type) {
	case NX_SCENE_NODE_TREE:
		return false;
	case NX_SCENE_NODE_RECT:
		return scene_rect_is_black_opaque(nx_scene_rect_from_node(node));
	case NX_SCENE_NODE_BUFFER:
		return scene_buffer_is_black_opaque(nx_scene_buffer_from_node(node));
	}

	return false;
}

static bool region_intersects_box(const pixman_region32_t *region,
		const struct nx_box *box) {
	pixman_region32_t intersection;
	pixman_region32_init(&intersection);
	pixman_region32_intersect_rect(&intersection, region,
		box->x, box->y, box->width, box->height);
	bool empty = pixman_region32_empty(&intersection);
	pixman_region32_fini(&intersection);
	return !empty;
}

static bool construct_render_list_iterator(struct nx_scene_node *node,
		int lx, int ly, void *_data) {
	struct render_list_constructor_data *data = _data;

	if (scene_node_invisible(node)) {
		return false;
	}

	float opacity = scene_node_inherited_opacity(node);

	// Alpha 0 draws nothing.
	if (node->type == NX_SCENE_NODE_BUFFER &&
			nx_scene_buffer_from_node(node)->opacity * opacity == 0.f) {
		return false;
	}

	// Fractional damage expansion: topmost only.
	bool may_cull_black = data->calculate_visibility &&
		(!data->fractional_scale || data->render_list->size == 0);
	// The background is black already.
	if (may_cull_black && scene_node_is_opaque_black(node, opacity)) {
		return false;
	}

	if (!region_intersects_box(&node->visible, &data->box)) {
		return false;
	}

	struct render_list_entry *entry = wl_array_add(data->render_list, sizeof(*entry));
	if (!entry) {
		return false;
	}

	*entry = (struct render_list_entry){
		.node = node,
		.x = lx,
		.y = ly,
		.opacity = opacity,
	};

	return false;
}

void scene_output_build_render_list(struct nx_scene_output *scene_output,
		const struct render_data *render_data) {
	struct render_list_constructor_data list_con = {
		.box = render_data->logical,
		.render_list = &scene_output->render_list,
		.calculate_visibility = scene_output->scene->calculate_visibility,
		.fractional_scale = floor(render_data->scale) != render_data->scale,
	};

	list_con.render_list->size = 0;
	scene_nodes_in_box(&scene_output->scene->tree.node, &list_con.box,
		construct_render_list_iterator, &list_con);
	array_realloc(list_con.render_list, list_con.render_list->size);
}
