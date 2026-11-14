#include <stdlib.h>
#include "platform/geometry/edges.h"
#include "platform/shell/layer/layer_shell.h"
#include "platform/scene/scene.h"

struct layer_axis {
	int bounds_pos, bounds_size;
	bool anchor_start, anchor_end;
	int margin_start, margin_end;
};

static void scene_layer_surface_handle_tree_destroy(
		struct wl_listener *listener, void *data) {
	struct nx_scene_layer_surface_v1 *scene_layer_surface =
		wl_container_of(listener, scene_layer_surface, tree_destroy);
	// The tree node owns the rest.
	wl_list_remove(&scene_layer_surface->tree_destroy.link);
	wl_list_remove(&scene_layer_surface->layer_surface_destroy.link);
	free(scene_layer_surface);
}

static void scene_layer_surface_handle_layer_surface_destroy(
		struct wl_listener *listener, void *data) {
	struct nx_scene_layer_surface_v1 *scene_layer_surface =
		wl_container_of(listener, scene_layer_surface, layer_surface_destroy);
	nx_scene_node_destroy(&scene_layer_surface->tree->node);
}

static void layer_surface_exclusive_zone(
		struct nx_layer_surface_v1_state *state,
		enum nx_edges edge,
		struct nx_box *usable_area) {
	switch (edge) {
	case NX_EDGE_NONE:
		return;
	case NX_EDGE_TOP:
		usable_area->y += state->exclusive_zone + state->margin.top;
		usable_area->height -= state->exclusive_zone + state->margin.top;
		break;
	case NX_EDGE_BOTTOM:
		usable_area->height -= state->exclusive_zone + state->margin.bottom;
		break;
	case NX_EDGE_LEFT:
		usable_area->x += state->exclusive_zone + state->margin.left;
		usable_area->width -= state->exclusive_zone + state->margin.left;
		break;
	case NX_EDGE_RIGHT:
		usable_area->width -= state->exclusive_zone + state->margin.right;
		break;
	}

	if (usable_area->width < 0) {
		usable_area->width = 0;
	}
	if (usable_area->height < 0) {
		usable_area->height = 0;
	}
}

// Zero size stretches; equal anchors center.
static void layer_axis_place(const struct layer_axis *axis, int *pos, int *size) {
	if (*size == 0) {
		*pos = axis->bounds_pos + axis->margin_start;
		*size = axis->bounds_size - (axis->margin_start + axis->margin_end);
		return;
	}

	if (axis->anchor_start == axis->anchor_end) {
		*pos = axis->bounds_pos + axis->bounds_size / 2 - *size / 2;
		return;
	}

	if (axis->anchor_start) {
		*pos = axis->bounds_pos + axis->margin_start;
		return;
	}

	*pos = axis->bounds_pos + axis->bounds_size - *size - axis->margin_end;
}

void nx_scene_layer_surface_v1_configure(
		struct nx_scene_layer_surface_v1 *scene_layer_surface,
		const struct nx_box *full_area, struct nx_box *usable_area) {
	struct nx_layer_surface_v1 *layer_surface =
		scene_layer_surface->layer_surface;
	struct nx_layer_surface_v1_state *state = &layer_surface->current;

	// Zone -1 ignores other zones.
	struct nx_box bounds = state->exclusive_zone == -1 ? *full_area : *usable_area;

	struct nx_box box = {
		.width = state->desired_width,
		.height = state->desired_height,
	};

	layer_axis_place(&(struct layer_axis){
		.bounds_pos = bounds.x,
		.bounds_size = bounds.width,
		.anchor_start = state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT,
		.anchor_end = state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT,
		.margin_start = state->margin.left,
		.margin_end = state->margin.right,
	}, &box.x, &box.width);
	layer_axis_place(&(struct layer_axis){
		.bounds_pos = bounds.y,
		.bounds_size = bounds.height,
		.anchor_start = state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP,
		.anchor_end = state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM,
		.margin_start = state->margin.top,
		.margin_end = state->margin.bottom,
	}, &box.y, &box.height);

	nx_scene_node_set_position(&scene_layer_surface->tree->node, box.x, box.y);
	nx_layer_surface_v1_configure(layer_surface, box.width, box.height);

	if (layer_surface->surface->mapped && state->exclusive_zone > 0) {
		enum nx_edges edge = nx_layer_surface_v1_get_exclusive_edge(layer_surface);
		layer_surface_exclusive_zone(state, edge, usable_area);
	}
}

struct nx_scene_layer_surface_v1 *nx_scene_layer_surface_v1_create(
		struct nx_scene_tree *parent,
		struct nx_layer_surface_v1 *layer_surface) {
	struct nx_scene_layer_surface_v1 *scene_layer_surface =
		calloc(1, sizeof(*scene_layer_surface));
	if (scene_layer_surface == NULL) {
		return NULL;
	}

	scene_layer_surface->layer_surface = layer_surface;

	scene_layer_surface->tree = nx_scene_tree_create(parent);
	if (scene_layer_surface->tree == NULL) {
		free(scene_layer_surface);
		return NULL;
	}

	struct nx_scene_tree *surface_tree = nx_scene_subsurface_tree_create(
		scene_layer_surface->tree, layer_surface->surface);
	if (surface_tree == NULL) {
		nx_scene_node_destroy(&scene_layer_surface->tree->node);
		free(scene_layer_surface);
		return NULL;
	}

	scene_layer_surface->tree_destroy.notify =
		scene_layer_surface_handle_tree_destroy;
	wl_signal_add(&scene_layer_surface->tree->node.events.destroy,
		&scene_layer_surface->tree_destroy);

	scene_layer_surface->layer_surface_destroy.notify =
		scene_layer_surface_handle_layer_surface_destroy;
	wl_signal_add(&layer_surface->events.destroy,
		&scene_layer_surface->layer_surface_destroy);

	return scene_layer_surface;
}
