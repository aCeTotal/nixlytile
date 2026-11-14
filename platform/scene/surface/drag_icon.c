#include <stdlib.h>
#include "platform/surface/compositor.h"
#include "platform/scene/scene.h"
#include "platform/clipboard/data_device/data_device.h"

struct scene_drag_icon {
	struct nx_scene_tree *tree;
	struct nx_scene_tree *surface_tree;
	struct nx_drag_icon *drag_icon;

	struct wl_listener tree_destroy;
	struct wl_listener drag_icon_surface_commit;
	struct wl_listener drag_icon_destroy;
};

static void drag_icon_handle_surface_commit(struct wl_listener *listener, void *data) {
	struct scene_drag_icon *icon =
		wl_container_of(listener, icon, drag_icon_surface_commit);
	struct nx_surface *surface = icon->drag_icon->surface;
	struct nx_scene_node *node = &icon->surface_tree->node;
	nx_scene_node_set_position(node,
		node->x + surface->current.dx, node->y + surface->current.dy);
}

static void drag_icon_handle_tree_destroy(struct wl_listener *listener, void *data) {
	struct scene_drag_icon *icon =
		wl_container_of(listener, icon, tree_destroy);
	wl_list_remove(&icon->tree_destroy.link);
	wl_list_remove(&icon->drag_icon_surface_commit.link);
	wl_list_remove(&icon->drag_icon_destroy.link);
	free(icon);
}

static void drag_icon_handle_destroy(struct wl_listener *listener, void *data) {
	struct scene_drag_icon *icon =
		wl_container_of(listener, icon, drag_icon_destroy);
	nx_scene_node_destroy(&icon->tree->node);
}

struct nx_scene_tree *nx_scene_drag_icon_create(
		struct nx_scene_tree *parent, struct nx_drag_icon *drag_icon) {
	struct scene_drag_icon *icon = calloc(1, sizeof(*icon));
	if (!icon) {
		return NULL;
	}

	icon->drag_icon = drag_icon;

	icon->tree = nx_scene_tree_create(parent);
	if (!icon->tree) {
		free(icon);
		return NULL;
	}

	icon->surface_tree = nx_scene_subsurface_tree_create(
		icon->tree, drag_icon->surface);
	if (!icon->surface_tree) {
		nx_scene_node_destroy(&icon->tree->node);
		free(icon);
		return NULL;
	}

	icon->tree_destroy.notify = drag_icon_handle_tree_destroy;
	wl_signal_add(&icon->tree->node.events.destroy, &icon->tree_destroy);
	icon->drag_icon_surface_commit.notify = drag_icon_handle_surface_commit;
	wl_signal_add(&drag_icon->surface->events.commit, &icon->drag_icon_surface_commit);
	icon->drag_icon_destroy.notify = drag_icon_handle_destroy;
	wl_signal_add(&drag_icon->events.destroy, &icon->drag_icon_destroy);

	return icon->tree;
}
