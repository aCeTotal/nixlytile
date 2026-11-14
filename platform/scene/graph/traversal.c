#include <math.h>
#include "platform/scene/graph/node.h"

struct box_walk {
	const struct nx_box *box;
	scene_node_box_iterator_func_t iterator;
	void *user_data;
};

struct buffer_walk {
	nx_scene_buffer_iterator_func_t iterator;
	void *user_data;
};

struct node_at_data {
	double lx, ly;
	double rx, ry;
	struct nx_scene_node *node;
};

// Topmost first, stops on true.
static bool nodes_in_box(struct nx_scene_node *node, int lx, int ly,
		const struct box_walk *walk) {
	if (!node->enabled) {
		return false;
	}

	if (node->type != NX_SCENE_NODE_TREE) {
		struct nx_box node_box = { .x = lx, .y = ly };
		scene_node_get_size(node, &node_box.width, &node_box.height);
		return nx_box_intersects(&node_box, walk->box) &&
			walk->iterator(node, lx, ly, walk->user_data);
	}

	struct nx_scene_tree *scene_tree = nx_scene_tree_from_node(node);
	struct nx_scene_node *child;
	wl_list_for_each_reverse(child, &scene_tree->children, link) {
		if (nodes_in_box(child, lx + child->x, ly + child->y, walk)) {
			return true;
		}
	}
	return false;
}

bool scene_nodes_in_box(struct nx_scene_node *node, const struct nx_box *box,
		scene_node_box_iterator_func_t iterator, void *user_data) {
	int x, y;
	nx_scene_node_coords(node, &x, &y);

	const struct box_walk walk = {
		.box = box,
		.iterator = iterator,
		.user_data = user_data,
	};
	return nodes_in_box(node, x, y, &walk);
}

static void for_each_buffer(struct nx_scene_node *node, int lx, int ly,
		const struct buffer_walk *walk) {
	if (!node->enabled) {
		return;
	}

	lx += node->x;
	ly += node->y;

	if (node->type == NX_SCENE_NODE_BUFFER) {
		walk->iterator(nx_scene_buffer_from_node(node), lx, ly, walk->user_data);
		return;
	}
	if (node->type != NX_SCENE_NODE_TREE) {
		return;
	}

	struct nx_scene_tree *scene_tree = nx_scene_tree_from_node(node);
	struct nx_scene_node *child;
	wl_list_for_each(child, &scene_tree->children, link) {
		for_each_buffer(child, lx, ly, walk);
	}
}

void nx_scene_node_for_each_buffer(struct nx_scene_node *node,
		nx_scene_buffer_iterator_func_t user_iterator, void *user_data) {
	const struct buffer_walk walk = {
		.iterator = user_iterator,
		.user_data = user_data,
	};
	for_each_buffer(node, 0, 0, &walk);
}

static bool scene_node_accepts_point(struct nx_scene_node *node,
		double *rx, double *ry) {
	if (node->type != NX_SCENE_NODE_BUFFER) {
		return true;
	}

	struct nx_scene_buffer *scene_buffer = nx_scene_buffer_from_node(node);
	return !scene_buffer->point_accepts_input ||
		scene_buffer->point_accepts_input(scene_buffer, rx, ry);
}

static bool scene_node_at_iterator(struct nx_scene_node *node,
		int lx, int ly, void *data) {
	struct node_at_data *at_data = data;

	double rx = at_data->lx - lx;
	double ry = at_data->ly - ly;
	if (!scene_node_accepts_point(node, &rx, &ry)) {
		return false;
	}

	at_data->rx = rx;
	at_data->ry = ry;
	at_data->node = node;
	return true;
}

struct nx_scene_node *nx_scene_node_at(struct nx_scene_node *node,
		double lx, double ly, double *nx, double *ny) {
	struct nx_box box = {
		.x = (int)floor(lx),
		.y = (int)floor(ly),
		.width = 1,
		.height = 1
	};

	struct node_at_data data = {
		.lx = lx,
		.ly = ly
	};

	if (!scene_nodes_in_box(node, &box, scene_node_at_iterator, &data)) {
		return NULL;
	}

	if (nx) {
		*nx = data.rx;
	}
	if (ny) {
		*ny = data.ry;
	}
	return data.node;
}
