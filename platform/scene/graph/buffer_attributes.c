#include <assert.h>
#include "platform/scene/graph/node.h"

void nx_scene_buffer_set_opaque_region(struct nx_scene_buffer *scene_buffer,
		const pixman_region32_t *region) {
	if (pixman_region32_equal(&scene_buffer->opaque_region, region)) {
		return;
	}

	pixman_region32_copy(&scene_buffer->opaque_region, region);

	int x, y;
	if (!nx_scene_node_coords(&scene_buffer->node, &x, &y)) {
		return;
	}

	pixman_region32_t update_region;
	pixman_region32_init(&update_region);
	scene_node_bounds(&scene_buffer->node, x, y, &update_region);
	scene_update_region(scene_node_get_root(&scene_buffer->node), &update_region);
	pixman_region32_fini(&update_region);
}

void nx_scene_buffer_set_source_box(struct nx_scene_buffer *scene_buffer,
		const struct nx_fbox *box) {
	if (nx_fbox_equal(&scene_buffer->src_box, box)) {
		return;
	}

	assert(box == NULL ||
		(box->x >= 0 && box->y >= 0 && box->width >= 0 && box->height >= 0));
	scene_buffer->src_box = box != NULL ? *box : (struct nx_fbox){0};

	scene_node_update(&scene_buffer->node, NULL);
}

void nx_scene_buffer_set_dest_size(struct nx_scene_buffer *scene_buffer,
		int width, int height) {
	if (scene_buffer->dst_width == width && scene_buffer->dst_height == height) {
		return;
	}

	assert(width >= 0 && height >= 0);
	scene_buffer->dst_width = width;
	scene_buffer->dst_height = height;
	scene_node_update(&scene_buffer->node, NULL);
}

void nx_scene_buffer_set_transform(struct nx_scene_buffer *scene_buffer,
		enum wl_output_transform transform) {
	if (scene_buffer->transform == transform) {
		return;
	}

	scene_buffer->transform = transform;
	scene_node_update(&scene_buffer->node, NULL);
}

void nx_scene_buffer_set_opacity(struct nx_scene_buffer *scene_buffer,
		float opacity) {
	if (scene_buffer->opacity == opacity) {
		return;
	}

	assert(opacity >= 0 && opacity <= 1);
	scene_buffer->opacity = opacity;
	scene_node_update(&scene_buffer->node, NULL);
}

void nx_scene_buffer_set_transfer_function(struct nx_scene_buffer *scene_buffer,
		enum nx_color_transfer_function transfer_function) {
	if (scene_buffer->transfer_function == transfer_function) {
		return;
	}

	scene_buffer->transfer_function = transfer_function;
	scene_node_update(&scene_buffer->node, NULL);
}

void nx_scene_buffer_set_primaries(struct nx_scene_buffer *scene_buffer,
		enum nx_color_named_primaries primaries) {
	if (scene_buffer->primaries == primaries) {
		return;
	}

	scene_buffer->primaries = primaries;
	scene_node_update(&scene_buffer->node, NULL);
}

void nx_scene_buffer_set_color_encoding(struct nx_scene_buffer *scene_buffer,
		enum nx_color_encoding color_encoding) {
	if (scene_buffer->color_encoding == color_encoding) {
		return;
	}

	scene_buffer->color_encoding = color_encoding;
	scene_node_update(&scene_buffer->node, NULL);
}

void nx_scene_buffer_set_color_range(struct nx_scene_buffer *scene_buffer,
		enum nx_color_range color_range) {
	if (scene_buffer->color_range == color_range) {
		return;
	}

	scene_buffer->color_range = color_range;
	scene_node_update(&scene_buffer->node, NULL);
}
