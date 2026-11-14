#include "platform/scene/graph/buffer.h"
#include "platform/scene/graph/node.h"
#include "platform/scene/output/scene_output.h"
#include "platform/xwayland/surface.h"

struct scene_update_data {
	pixman_region32_t *visible;
	const pixman_region32_t *update_region;
	struct nx_box update_box;
	struct wl_list *outputs;
	bool calculate_visibility;

	struct nx_xwayland_surface *restack_above;
};

// X11 stacking follows the scene.
static struct nx_xwayland_surface *scene_node_managed_xwayland_surface(
		struct nx_scene_node *node) {
	if (node->type != NX_SCENE_NODE_BUFFER) {
		return NULL;
	}

	struct nx_scene_buffer *buffer_node = nx_scene_buffer_from_node(node);
	struct nx_scene_surface *surface_node = nx_scene_surface_try_from_buffer(buffer_node);
	if (!surface_node) {
		return NULL;
	}

	struct nx_xwayland_surface *xwayland_surface =
		nx_xwayland_surface_try_from_nx_surface(surface_node->surface);
	if (!xwayland_surface || xwayland_surface->override_redirect) {
		return NULL;
	}

	return xwayland_surface;
}

static void restack_xwayland_surface(struct nx_scene_node *node,
		const struct nx_box *box, struct scene_update_data *data) {
	struct nx_xwayland_surface *xwayland_surface =
		scene_node_managed_xwayland_surface(node);
	if (!xwayland_surface) {
		return;
	}

	// Partially updated nodes keep their place.
	if (nx_box_contains_box(&data->update_box, box)) {
		nx_xwayland_surface_restack(xwayland_surface, data->restack_above,
			data->restack_above ? XCB_STACK_MODE_BELOW : XCB_STACK_MODE_ABOVE);
	}

	data->restack_above = xwayland_surface;
}

static bool scene_node_update_iterator(struct nx_scene_node *node,
		int lx, int ly, void *_data) {
	struct scene_update_data *data = _data;

	struct nx_box box = { .x = lx, .y = ly };
	scene_node_get_size(node, &box.width, &box.height);

	pixman_region32_subtract(&node->visible, &node->visible, data->update_region);
	pixman_region32_union(&node->visible, &node->visible, data->visible);
	pixman_region32_intersect_rect(&node->visible, &node->visible,
		lx, ly, box.width, box.height);

	if (data->calculate_visibility && scene_node_inherited_opacity(node) == 1) {
		pixman_region32_t opaque;
		pixman_region32_init(&opaque);
		scene_node_opaque_region(node, lx, ly, &opaque);
		pixman_region32_subtract(data->visible, data->visible, &opaque);
		pixman_region32_fini(&opaque);
	}

	scene_node_update_outputs(node, data->outputs, NULL, NULL);
	restack_xwayland_surface(node, &box, data);

	return false;
}

void scene_update_region(struct nx_scene *scene,
		const pixman_region32_t *update_region) {
	pixman_region32_t visible;
	pixman_region32_init(&visible);
	pixman_region32_copy(&visible, update_region);

	struct pixman_box32 *region_box = pixman_region32_extents(update_region);
	struct scene_update_data data = {
		.visible = &visible,
		.update_region = update_region,
		.update_box = {
			.x = region_box->x1,
			.y = region_box->y1,
			.width = region_box->x2 - region_box->x1,
			.height = region_box->y2 - region_box->y1,
		},
		.outputs = &scene->outputs,
		.calculate_visibility = scene->calculate_visibility,
	};

	scene_nodes_in_box(&scene->tree.node, &data.update_box, scene_node_update_iterator, &data);

	pixman_region32_fini(&visible);
}

// Stale fences would block rendering.
static void scene_buffer_release_disabled(struct nx_scene_buffer *scene_buffer) {
	// Only surfaces can restore their buffer.
	if (nx_scene_surface_try_from_buffer(scene_buffer) != NULL) {
		scene_buffer_set_buffer(scene_buffer, NULL);
		scene_buffer_set_texture(scene_buffer, NULL);
	}
	scene_buffer_set_wait_timeline(scene_buffer, NULL, 0);
}

static void scene_leaf_cleanup_when_disabled(struct nx_scene_node *node,
		struct wl_list *outputs) {
	pixman_region32_clear(&node->visible);
	scene_node_update_outputs(node, outputs, NULL, NULL);

	if (node->type == NX_SCENE_NODE_BUFFER) {
		scene_buffer_release_disabled(nx_scene_buffer_from_node(node));
	}

	struct nx_xwayland_surface *xwayland_surface =
		scene_node_managed_xwayland_surface(node);
	if (!xwayland_surface) {
		return;
	}

	nx_xwayland_surface_restack(xwayland_surface, NULL, XCB_STACK_MODE_BELOW);
}

static void scene_node_cleanup_when_disabled(struct nx_scene_node *node,
		struct wl_list *outputs) {
	if (node->type != NX_SCENE_NODE_TREE) {
		scene_leaf_cleanup_when_disabled(node, outputs);
		return;
	}

	struct nx_scene_tree *scene_tree = nx_scene_tree_from_node(node);
	struct nx_scene_node *child;
	wl_list_for_each(child, &scene_tree->children, link) {
		if (child->enabled) {
			scene_node_cleanup_when_disabled(child, outputs);
		}
	}
}

// Explicit damage: just disabled.
static void scene_node_update_disabled(struct nx_scene *scene,
		struct nx_scene_node *node, pixman_region32_t *damage) {
	if (!damage) {
		return;
	}

	scene_node_cleanup_when_disabled(node, &scene->outputs);

	scene_update_region(scene, damage);
	scene_damage_outputs(scene, damage);
	pixman_region32_fini(damage);
}

// NULL damage means the current visibility.
void scene_node_update(struct nx_scene_node *node,
		pixman_region32_t *damage) {
	struct nx_scene *scene = scene_node_get_root(node);

	int x, y;
	if (!nx_scene_node_coords(node, &x, &y)) {
		scene_node_update_disabled(scene, node, damage);
		return;
	}

	pixman_region32_t visible;
	if (!damage) {
		pixman_region32_init(&visible);
		scene_node_visibility(node, &visible);
		damage = &visible;
	}

	pixman_region32_t update_region;
	pixman_region32_init(&update_region);
	pixman_region32_copy(&update_region, damage);
	scene_node_bounds(node, x, y, &update_region);

	scene_update_region(scene, &update_region);
	pixman_region32_fini(&update_region);

	scene_node_visibility(node, damage);
	scene_damage_outputs(scene, damage);
	pixman_region32_fini(damage);
}
