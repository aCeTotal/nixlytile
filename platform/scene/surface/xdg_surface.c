#include <stdlib.h>
#include "platform/scene/scene.h"
#include "platform/shell/xdg/xdg_shell.h"

struct scene_xdg_surface {
	struct nx_scene_tree *tree;
	struct nx_xdg_surface *xdg_surface;
	struct nx_scene_tree *surface_tree;

	struct wl_listener tree_destroy;
	struct wl_listener xdg_surface_destroy;
	struct wl_listener xdg_surface_commit;
};

static void scene_xdg_surface_handle_tree_destroy(struct wl_listener *listener,
		void *data) {
	struct scene_xdg_surface *scene_xdg_surface =
		wl_container_of(listener, scene_xdg_surface, tree_destroy);
	// The tree node owns the rest.
	wl_list_remove(&scene_xdg_surface->tree_destroy.link);
	wl_list_remove(&scene_xdg_surface->xdg_surface_destroy.link);
	wl_list_remove(&scene_xdg_surface->xdg_surface_commit.link);
	free(scene_xdg_surface);
}

static void scene_xdg_surface_handle_xdg_surface_destroy(struct wl_listener *listener,
		void *data) {
	struct scene_xdg_surface *scene_xdg_surface =
		wl_container_of(listener, scene_xdg_surface, xdg_surface_destroy);
	nx_scene_node_destroy(&scene_xdg_surface->tree->node);
}

static void scene_xdg_surface_update_position(
		struct scene_xdg_surface *scene_xdg_surface) {
	struct nx_xdg_surface *xdg_surface = scene_xdg_surface->xdg_surface;

	nx_scene_node_set_position(&scene_xdg_surface->surface_tree->node,
		-xdg_surface->geometry.x, -xdg_surface->geometry.y);

	if (xdg_surface->role != NX_XDG_SURFACE_ROLE_POPUP || xdg_surface->popup == NULL) {
		return;
	}

	struct nx_xdg_popup *popup = xdg_surface->popup;
	nx_scene_node_set_position(&scene_xdg_surface->tree->node,
		popup->current.geometry.x, popup->current.geometry.y);
}

static void scene_xdg_surface_handle_xdg_surface_commit(struct wl_listener *listener,
		void *data) {
	struct scene_xdg_surface *scene_xdg_surface =
		wl_container_of(listener, scene_xdg_surface, xdg_surface_commit);
	scene_xdg_surface_update_position(scene_xdg_surface);
}

struct nx_scene_tree *nx_scene_xdg_surface_create(
		struct nx_scene_tree *parent, struct nx_xdg_surface *xdg_surface) {
	struct scene_xdg_surface *scene_xdg_surface =
		calloc(1, sizeof(*scene_xdg_surface));
	if (scene_xdg_surface == NULL) {
		return NULL;
	}

	scene_xdg_surface->xdg_surface = xdg_surface;

	scene_xdg_surface->tree = nx_scene_tree_create(parent);
	if (scene_xdg_surface->tree == NULL) {
		free(scene_xdg_surface);
		return NULL;
	}

	scene_xdg_surface->surface_tree = nx_scene_subsurface_tree_create(
		scene_xdg_surface->tree, xdg_surface->surface);
	if (scene_xdg_surface->surface_tree == NULL) {
		nx_scene_node_destroy(&scene_xdg_surface->tree->node);
		free(scene_xdg_surface);
		return NULL;
	}

	scene_xdg_surface->tree_destroy.notify =
		scene_xdg_surface_handle_tree_destroy;
	wl_signal_add(&scene_xdg_surface->tree->node.events.destroy,
		&scene_xdg_surface->tree_destroy);

	scene_xdg_surface->xdg_surface_destroy.notify =
		scene_xdg_surface_handle_xdg_surface_destroy;
	wl_signal_add(&xdg_surface->events.destroy, &scene_xdg_surface->xdg_surface_destroy);

	scene_xdg_surface->xdg_surface_commit.notify =
		scene_xdg_surface_handle_xdg_surface_commit;
	wl_signal_add(&xdg_surface->surface->events.commit,
		&scene_xdg_surface->xdg_surface_commit);

	scene_xdg_surface_update_position(scene_xdg_surface);

	return scene_xdg_surface->tree;
}
