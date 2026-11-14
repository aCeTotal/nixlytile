#include <assert.h>
#include <stdlib.h>
#include "platform/os/env.h"
#include "platform/scene/graph/node.h"

static void scene_tree_init(struct nx_scene_tree *tree,
		struct nx_scene_tree *parent) {
	*tree = (struct nx_scene_tree){ .opacity = 1 };
	scene_node_init(&tree->node, NX_SCENE_NODE_TREE, parent);
	wl_list_init(&tree->children);
}

struct nx_scene *nx_scene_create(void) {
	struct nx_scene *scene = calloc(1, sizeof(*scene));
	if (scene == NULL) {
		return NULL;
	}

	scene_tree_init(&scene->tree, NULL);

	wl_list_init(&scene->outputs);
	wl_list_init(&scene->linux_dmabuf_v1_destroy.link);
	wl_list_init(&scene->gamma_control_manager_v1_destroy.link);
	wl_list_init(&scene->gamma_control_manager_v1_set_gamma.link);

	const char *debug_damage_options[] = {
		"none",
		"rerender",
		"highlight",
		NULL
	};

	scene->debug_damage_option = env_parse_switch("NIXLY_SCENE_DEBUG_DAMAGE", debug_damage_options);
	scene->direct_scanout = !env_parse_bool("NIXLY_SCENE_DISABLE_DIRECT_SCANOUT");
	scene->calculate_visibility = !env_parse_bool("NIXLY_SCENE_DISABLE_VISIBILITY");
	scene->highlight_transparent_region = env_parse_bool("NIXLY_SCENE_HIGHLIGHT_TRANSPARENT_REGION");

	return scene;
}

struct nx_scene_tree *nx_scene_tree_create(struct nx_scene_tree *parent) {
	assert(parent);

	struct nx_scene_tree *tree = calloc(1, sizeof(*tree));
	if (tree == NULL) {
		return NULL;
	}

	scene_tree_init(tree, parent);
	return tree;
}

void nx_scene_tree_set_opacity(struct nx_scene_tree *tree, float opacity) {
	if (tree->opacity == opacity) {
		return;
	}

	assert(opacity >= 0 && opacity <= 1);
	tree->opacity = opacity;
	scene_node_update(&tree->node, NULL);
}
