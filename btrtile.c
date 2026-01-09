/* ************************************************************************** */
/*                                                                            */
/*                                                        :::      ::::::::   */
/*   btrtile.c                                          :+:      :+:    :+:   */
/*                                                    +:+ +:+         +:+     */
/*   By: jmakkone <jmakkone@student.hive.fi>        +#+  +:+       +#+        */
/*                                                +#+#+#+#+#+   +#+           */
/*   Created: 2024/12/15 00:26:07 by jmakkone          #+#    #+#             */
/*   Updated: 2025/02/13 23:25:03 by jmakkone         ###   ########.fr       */
/*                                                                            */
/* ************************************************************************** */

typedef struct LayoutNode {
	unsigned int is_client_node;
	unsigned int is_split_vertically;
	float split_ratio;
	struct LayoutNode *left;
	struct LayoutNode *right;
	struct LayoutNode *split_node;
	Client *client;
} LayoutNode;

static void apply_layout(Monitor *m, LayoutNode *node,
						struct wlr_box area, unsigned int is_root);
static void btrtile(Monitor *m);
static LayoutNode *create_client_node(Client *c);
static LayoutNode *create_split_node(unsigned int is_split_vertically,
									LayoutNode *left, LayoutNode *right);
static void destroy_node(LayoutNode *node);
static void destroy_tree(Monitor *m);
static LayoutNode *find_client_node(LayoutNode *node, Client *c);
static LayoutNode *find_suitable_split(LayoutNode *start, unsigned int need_vert);
static void init_tree(Monitor *m);
static int insert_client(Monitor *m, Client *focused_client, Client *new_client);
static void insert_client_at(Monitor *m, Client *target, Client *new_client, double cx, double cy);
static LayoutNode *remove_client_node(LayoutNode *node, Client *c);
static void remove_client(Monitor *m, Client *c);
static void focusdir(const Arg *arg);
static void setratio_h(const Arg *arg);
static void setratio_v(const Arg *arg);
static void swapclients(const Arg *arg);
static int collect_columns(LayoutNode *node, Monitor *m, LayoutNode **out_nodes,
		unsigned int *out_counts, Client **out_clients, int max_out);
static Client *first_visible_client(LayoutNode *node, Monitor *m);
static Client *first_active_client(LayoutNode *node, Monitor *m);
static Client *pick_target_client(Monitor *m, Client *focused_client);
static unsigned int count_columns(LayoutNode *node, Monitor *m);
static unsigned int visible_count(LayoutNode *node, Monitor *m);
static unsigned int placement_count(LayoutNode *node, Monitor *m);
static unsigned int target_columns(Monitor *m);
static Client *xytoclient(double x, double y);
static void start_tile_drag(Monitor *m, Client *c);
static void end_tile_drag(void);
static void swap_columns(Monitor *m, Client *c1, Client *c2);
static LayoutNode *find_column_root(LayoutNode *node, Monitor *m);
static unsigned int count_in_column(LayoutNode *col_node, Monitor *m);
static LayoutNode *find_column_half(LayoutNode *node, Monitor *m);
static int same_column(Monitor *m, Client *c1, Client *c2);
static int can_move_tile(Monitor *m, Client *source, Client *target);
static void swap_tiles_in_tree(Monitor *m, Client *c1, Client *c2);

static int resizing_from_mouse = 0;
static int split_side_toggle = 0;
static int col_pick_toggle = 0;

/* Get the index of the primary (lowest set) tag bit */
static unsigned int
get_tag_index(uint32_t tags)
{
	unsigned int i;
	for (i = 0; i < TAGCOUNT; i++) {
		if (tags & (1u << i))
			return i;
	}
	return 0;
}

/* Get current tag's root */
static LayoutNode **
get_current_root(Monitor *m)
{
	if (m)
		return &m->root[get_tag_index(m->tagset[m->seltags])];
	return NULL;
}

/* Count tiled clients on a specific tag */
static unsigned int
count_tiles_on_tag(Monitor *m, uint32_t tag)
{
	Client *c;
	unsigned int count = 0;

	wl_list_for_each(c, &clients, link) {
		if (c->mon == m && (c->tags & tag) && !c->isfloating && !c->isfullscreen)
			count++;
	}
	return count;
}

/* Find an available tag with space for more tiles */
static uint32_t
find_available_tag(Monitor *m)
{
	unsigned int i;
	uint32_t tag;
	unsigned int desired_cols = target_columns(m);
	unsigned int max_tiles = desired_cols * 4;

	/* Start from tag 1, find first tag with space */
	for (i = 0; i < TAGCOUNT; i++) {
		tag = 1u << i;
		if (count_tiles_on_tag(m, tag) < max_tiles)
			return tag;
	}
	/* All tags full, return 0 */
	return 0;
}
static double resize_last_update_x __attribute__((unused)),
             resize_last_update_y __attribute__((unused));
static uint32_t last_resize_time __attribute__((unused)) = 0;

/* Drag state tracking */
static Client *dragging_client = NULL;
static LayoutNode *drag_source_node = NULL;
static int drag_was_alone_in_column = 0;
static struct wlr_box drag_placeholder_box = {0};

static void
apply_layout(Monitor *m, LayoutNode *node,
             struct wlr_box area, unsigned int is_root)
{
	Client *c;
	float ratio;
	unsigned int left_count, right_count, mid, e = m->gaps;
	struct wlr_box left_area, right_area;

	if (!node)
		return;

	if (is_root && e) {
		area.x += gappx;
		area.y += gappx;
		area.width -= 2 * gappx;
		area.height -= 2 * gappx;
	}

	/* If this node is a client node, check if it is visible. */
	if (node->is_client_node) {
		c = node->client;
		if (!c)
			return;
		/* Skip dragging client but reserve its space */
		if (c == dragging_client) {
			drag_placeholder_box = area;
			return;
		}
		if (!VISIBLEON(c, m) || c->isfloating || c->isfullscreen)
			return;
		resize(c, area, 0);
		c->old_geom = area;
		return;
	}

	/* For a split node, we see how many visible children are on each side: */
	left_count  = visible_count(node->left, m);
	right_count = visible_count(node->right, m);

	if (left_count == 0 && right_count == 0) {
		return;
	} else if (left_count > 0 && right_count == 0) {
		apply_layout(m, node->left, area, 0);
		return;
	} else if (left_count == 0 && right_count > 0) {
		apply_layout(m, node->right, area, 0);
		return;
	}

	/* If we’re here, we have visible clients in both subtrees. */
	ratio = node->split_ratio;
	if (ratio < 0.05f)
		ratio = 0.05f;
	if (ratio > 0.95f)
		ratio = 0.95f;

	memset(&left_area, 0, sizeof(left_area));
	memset(&right_area, 0, sizeof(right_area));

	if (node->is_split_vertically) {
		mid = (unsigned int)(area.width * ratio);
		left_area.x      = area.x;
		left_area.y      = area.y;
		left_area.width  = mid;
		left_area.height = area.height;

		right_area.x      = area.x + mid;
		right_area.y      = area.y;
		right_area.width  = area.width  - mid;
		right_area.height = area.height;

		if (e) {
			left_area.width -= gappx / 2;
			right_area.x += gappx / 2;
			right_area.width -= gappx / 2;
		}
	} else {
		/* horizontal split */
		mid = (unsigned int)(area.height * ratio);
		left_area.x     = area.x;
		left_area.y     = area.y;
		left_area.width = area.width;
		left_area.height = mid;

		right_area.x     = area.x;
		right_area.y     = area.y + mid;
		right_area.width = area.width;
		right_area.height= area.height - mid;

		if (e) {
			left_area.height -= gappx / 2;
			right_area.y += gappx / 2;
			right_area.height -= gappx / 2;
		}
	}

	apply_layout(m, node->left,  left_area,  0);
	apply_layout(m, node->right, right_area, 0);
}

static void
btrtile(Monitor *m)
{
	Client *c;
	int n = 0;
	LayoutNode *found;
	struct wlr_box full_area;
	LayoutNode **root;

	if (!m)
		return;

	root = get_current_root(m);

	/* Remove non tiled clients from tree (but keep dragging_client). */
	if (*root) {
		wl_list_for_each(c, &clients, link) {
			if (c == dragging_client && c->mon == m)
				continue; /* Keep dragging client in tree to preserve space */
			if (c->mon == m && !c->isfloating && !c->isfullscreen) {
			} else {
				remove_client(m, c);
			}
		}
	}

	/* Insert visible clients that are not part of the tree. */
	wl_list_for_each(c, &clients, link) {
		/* Count dragging_client to keep layout active */
		if (c == dragging_client && c->mon == m) {
			n++;
			continue;
		}
		if (VISIBLEON(c, m) && !c->isfloating && !c->isfullscreen && c->mon == m) {
			found = *root ? find_client_node(*root, c) : NULL;
			if (!found) {
				if (!insert_client(m, NULL, c)) {
					/* Workspace is full - move client to next available tag */
					uint32_t available_tag = find_available_tag(m);
					if (available_tag && available_tag != (c->tags & m->tagset[m->seltags])) {
						c->tags = available_tag;
						/* Client is now on different tag, skip it */
						continue;
					}
					/* No available tag - make floating as last resort */
					c->isfloating = 1;
					continue;
				}
			}
			n++;
		}
	}

	if (n == 0 || !*root)
		return;

	full_area = m->w;
	apply_layout(m, *root, full_area, 1);
}

static LayoutNode *
create_client_node(Client *c)
{
	LayoutNode *node = calloc(1, sizeof(LayoutNode));

	if (!node)
		return NULL;
	node->is_client_node = 1;
	node->split_ratio = 0.5f;
	node->client = c;
	return node;
}

static LayoutNode *
create_split_node(unsigned int is_split_vertically,
				LayoutNode *left, LayoutNode *right)
{
	LayoutNode *node = calloc(1, sizeof(LayoutNode));

	if (!node)
		return NULL;
	node->is_client_node = 0;
	node->split_ratio = 0.5f;
	node->is_split_vertically = is_split_vertically;
	node->left = left;
	node->right = right;
	if (left)
		left->split_node = node;
	if (right)
		right->split_node = node;
	return node;
}

static void
destroy_node(LayoutNode *node)
{
	if (!node)
		return;
	if (!node->is_client_node) {
		destroy_node(node->left);
		destroy_node(node->right);
	}
	free(node);
}

static void
destroy_tree(Monitor *m)
{
	unsigned int i;
	if (!m)
		return;
	for (i = 0; i < TAGCOUNT; i++) {
		if (m->root[i]) {
			destroy_node(m->root[i]);
			m->root[i] = NULL;
		}
	}
}

static LayoutNode *
find_client_node(LayoutNode *node, Client *c)
{
	LayoutNode *res;

	if (!node || !c)
		return NULL;
	if (node->is_client_node) {
		return (node->client == c) ? node : NULL;
	}
	res = find_client_node(node->left, c);
	return res ? res : find_client_node(node->right, c);
}

static LayoutNode *
find_suitable_split(LayoutNode *start_node, unsigned int need_vertical)
{
	LayoutNode *n = start_node;
	/* if we started from a client node, jump to its parent: */
	if (n && n->is_client_node)
		n = n->split_node;

	while (n) {
		if (!n->is_client_node && n->is_split_vertically == need_vertical &&
			visible_count(n->left, selmon) > 0 && visible_count(n->right, selmon) > 0)
			return n;
		n = n->split_node;
	}
	return NULL;
}

static void
init_tree(Monitor *m)
{
	unsigned int i;
	if (!m)
		return;
	/* Start with NULL roots - first client on each tag will become the root */
	for (i = 0; i < TAGCOUNT; i++)
		m->root[i] = NULL;
}

static int
insert_client(Monitor *m, Client *focused_client, Client *new_client)
{
	Client *old_client;
	LayoutNode **root, *old_root,
	*focused_node, *new_client_node, *old_client_node;
	unsigned int wider;
	int place_new_first;
	unsigned int desired_cols, current_cols, total_tiles, max_tiles;
	Client *target_client;

	root = get_current_root(m);
	desired_cols = target_columns(m);
	current_cols = count_columns(*root, m);

	/* Check if workspace is full (max 4 tiles per column) */
	total_tiles = visible_count(*root, m);
	max_tiles = desired_cols * 4;
	if (total_tiles >= max_tiles) {
		/* Workspace is full - don't allow more tiles */
		return 0;
	}

	target_client = focused_client;
	if (!target_client)
		target_client = pick_target_client(m, NULL);
	if (!target_client)
		target_client = focused_client;
	focused_client = target_client;

	/* If no root , new client becomes the root. */
	if (!*root) {
		*root = create_client_node(new_client);
		return 1;
	}

	/* Find the focused_client node,
	 * if not found split the root. */
	focused_node = focused_client ?
		find_client_node(*root, focused_client) : NULL;
	if (!focused_node) {
		old_root = *root;
		new_client_node = create_client_node(new_client);
		/* Vertical split for new column, horizontal for tiles within column */
		if (current_cols < desired_cols)
			*root = create_split_node(1, old_root, new_client_node);
		else
			*root = create_split_node(0, old_root, new_client_node);
		return 1;
	}

	/* Turn focused node from a client node into a split node,
	 * and attach old_client + new_client. */
	old_client = focused_node->client;
	old_client_node = create_client_node(old_client);
	new_client_node = create_client_node(new_client);

	/* Split direction:
	 * - Need more columns? -> vertical split (create column)
	 * - Tiles within column -> always horizontal split (top/bottom)
	 * - 4 tiles in column -> FULL, don't allow more */
	if (current_cols < desired_cols) {
		wider = 1; /* Need more columns - vertical split */
	} else {
		LayoutNode *col_root = find_column_root(focused_node, m);
		unsigned int col_tiles = count_in_column(col_root, m);

		if (col_tiles >= 4) {
			/* Column is full - don't allow more tiles */
			free(old_client_node);
			free(new_client_node);
			return 0;
		} else if (col_tiles == 1) {
			wider = 0; /* First split in column: horizontal (top/bottom) */
		} else {
			/* Check if target half (top/bottom) is full (max 2 tiles per half) */
			LayoutNode *half = find_column_half(focused_node, m);
			if (half && visible_count(half, m) >= 2) {
				/* This half already has 2 tiles - block */
				free(old_client_node);
				free(new_client_node);
				return 0;
			}
			wider = 1; /* After horizontal split: vertical (left/right) */
		}
	}
	focused_node->is_client_node = 0;
	focused_node->client         = NULL;
	focused_node->is_split_vertically = (wider ? 1 : 0);

	/* Pick side deterministically to balance columns, not cursor position. */
	place_new_first = 0;
	if (focused_node->is_split_vertically) {
		unsigned int old_count = visible_count(old_client_node, m);
		unsigned int new_count = visible_count(new_client_node, m);
		if (old_count > new_count)
			place_new_first = 1;
		else if (old_count == new_count)
			place_new_first = split_side_toggle;
	} else {
		unsigned int old_count = visible_count(old_client_node, m);
		unsigned int new_count = visible_count(new_client_node, m);
		if (old_count > new_count)
			place_new_first = 1;
		else if (old_count == new_count)
			place_new_first = split_side_toggle;
	}
	split_side_toggle = !split_side_toggle;

	if (place_new_first) {
		focused_node->left  = new_client_node;
		focused_node->right = old_client_node;
	} else {
		focused_node->left  = old_client_node;
		focused_node->right = new_client_node;
	}
	old_client_node->split_node = focused_node;
	new_client_node->split_node = focused_node;
	focused_node->split_ratio = 0.5f;
	return 1;
}

static void
insert_client_at(Monitor *m, Client *target, Client *new_client, double cx, double cy)
{
	Client *old_client;
	LayoutNode **root, *old_root,
		*target_node, *new_client_node, *old_client_node;
	unsigned int wider;
	int place_new_first;
	unsigned int desired_cols, current_cols;
	int target_center_x, target_center_y;

	if (!target || !new_client) {
		insert_client(m, target, new_client);
		return;
	}

	root = get_current_root(m);
	desired_cols = target_columns(m);
	current_cols = count_columns(*root, m);

	/* If no root, new client becomes the root. */
	if (!*root) {
		*root = create_client_node(new_client);
		return;
	}

	/* Find the target node */
	target_node = find_client_node(*root, target);
	if (!target_node) {
		old_root = *root;
		new_client_node = create_client_node(new_client);
		/* Vertical split for new column, horizontal for tiles within column */
		if (current_cols < desired_cols)
			*root = create_split_node(1, old_root, new_client_node);
		else
			*root = create_split_node(0, old_root, new_client_node);
		return;
	}

	/* Turn target node from a client node into a split node */
	old_client = target_node->client;
	old_client_node = create_client_node(old_client);
	new_client_node = create_client_node(new_client);

	/* Determine placement based on cursor position relative to target center */
	target_center_x = target->geom.x + target->geom.width / 2;
	target_center_y = target->geom.y + target->geom.height / 2;

	/* Split direction:
	 * - Need more columns? -> vertical split (create column)
	 * - Tiles within column -> always horizontal split (top/bottom)
	 * - 4 tiles in column -> FULL, don't allow more */
	if (current_cols < desired_cols) {
		wider = 1; /* Need more columns - vertical split */
	} else {
		LayoutNode *col_root = find_column_root(target_node, m);
		unsigned int col_tiles = count_in_column(col_root, m);

		if (col_tiles >= 4) {
			/* Column is full - don't allow more tiles */
			free(old_client_node);
			free(new_client_node);
			/* Re-insert at original position */
			insert_client(m, NULL, new_client);
			return;
		} else if (col_tiles == 1) {
			wider = 0; /* First split in column: horizontal (top/bottom) */
		} else {
			/* Check if target half (top/bottom) is full (max 2 tiles per half) */
			LayoutNode *half = find_column_half(target_node, m);
			if (half && visible_count(half, m) >= 2) {
				/* This half already has 2 tiles - block */
				free(old_client_node);
				free(new_client_node);
				/* Re-insert at original position */
				insert_client(m, NULL, new_client);
				return;
			}
			wider = 1; /* After horizontal split: vertical (left/right) */
		}
	}

	target_node->is_client_node = 0;
	target_node->client = NULL;
	target_node->is_split_vertically = (wider ? 1 : 0);

	/* Placement based on cursor position */
	if (target_node->is_split_vertically) {
		/* Vertical split: left/right based on cursor x */
		place_new_first = (cx < target_center_x);
	} else {
		/* Horizontal split: top/bottom based on cursor y */
		place_new_first = (cy < target_center_y);
	}

	if (place_new_first) {
		target_node->left = new_client_node;
		target_node->right = old_client_node;
	} else {
		target_node->left = old_client_node;
		target_node->right = new_client_node;
	}
	old_client_node->split_node = target_node;
	new_client_node->split_node = target_node;
	target_node->split_ratio = 0.5f;
}

static LayoutNode *
remove_client_node(LayoutNode *node, Client *c)
{
	LayoutNode *tmp;
	if (!node)
		return NULL;
	if (node->is_client_node) {
		/* If this client_node is the client we're removing,
		 * return NULL to remove it */
		if (node->client == c) {
			free(node);
			return NULL;
		}
		return node;
	}

	node->left = remove_client_node(node->left, c);
	node->right = remove_client_node(node->right, c);

	/* If one of the client node is NULL after removal and the other is not,
	 * we "lift" the other client node up to replace this split node. */
	if (!node->left && node->right) {
		tmp = node->right;

		/* Save pointer to split node */
		if (tmp)
			tmp->split_node = node->split_node;

		/* If lifted node is a vertical split becoming a column root,
		 * convert to horizontal so tiles stack vertically */
		if (tmp && !tmp->is_client_node && tmp->is_split_vertically) {
			/* It's a column root if: no parent (becomes tree root) OR
			 * parent is a vertical split (column separator) */
			if (!tmp->split_node ||
			    (tmp->split_node && tmp->split_node->is_split_vertically)) {
				tmp->is_split_vertically = 0;
			}
		}

		free(node);
		return tmp;
	}

	if (!node->right && node->left) {
		tmp = node->left;

		/* Save pointer to split node */
		if (tmp)
			tmp->split_node = node->split_node;

		/* If lifted node is a vertical split becoming a column root,
		 * convert to horizontal so tiles stack vertically */
		if (tmp && !tmp->is_client_node && tmp->is_split_vertically) {
			if (!tmp->split_node ||
			    (tmp->split_node && tmp->split_node->is_split_vertically)) {
				tmp->is_split_vertically = 0;
			}
		}

		free(node);
		return tmp;
	}

	/* If both children exist or both are NULL (empty tree),
	 * return node as is. */
	return node;
}

static void
remove_client(Monitor *m, Client *c)
{
	unsigned int i;
	if (!m || !c)
		return;
	/* Remove client from all tag trees it might be in */
	for (i = 0; i < TAGCOUNT; i++) {
		if (m->root[i])
			m->root[i] = remove_client_node(m->root[i], c);
	}
}

static void
setratio_h(const Arg *arg)
{
	Client *sel = focustop(selmon);
	LayoutNode *client_node, *split_node;
	float new_ratio;

	if (!sel || !selmon || !selmon->lt[selmon->sellt]->arrange)
		return;

	client_node = find_client_node(*get_current_root(selmon), sel);
	if (!client_node)
		return;

	split_node = find_suitable_split(client_node, 1);
	if (!split_node)
		return;

	new_ratio = (arg->f != 0.0f) ? (split_node->split_ratio + arg->f) : 0.5f;
	if (new_ratio < 0.05f)
		new_ratio = 0.05f;
	if (new_ratio > 0.95f)
		new_ratio = 0.95f;
	split_node->split_ratio = new_ratio;

	/* Skip the arrange if done resizing by mouse,
	 * we call arrange from motionotify */
	if (!resizing_from_mouse) {
		arrange(selmon);
	}
}

static void
setratio_v(const Arg *arg)
{
	Client *sel = focustop(selmon);
	LayoutNode *client_node, *split_node;
	float new_ratio;

	if (!sel || !selmon || !selmon->lt[selmon->sellt]->arrange)
		return;

	client_node = find_client_node(*get_current_root(selmon), sel);
	if (!client_node)
		return;

	split_node = find_suitable_split(client_node, 0);
	if (!split_node)
		return;

	new_ratio = (arg->f != 0.0f) ? (split_node->split_ratio + arg->f) : 0.5f;
	if (new_ratio < 0.05f)
		new_ratio = 0.05f;
	if (new_ratio > 0.95f)
		new_ratio = 0.95f;
	split_node->split_ratio = new_ratio;

	/* Skip the arrange if done resizing by mouse,
	 * we call arrange from motionotify */
	if (!resizing_from_mouse) {
		arrange(selmon);
	}
}

static void swapclients(const Arg *arg) {
	Client *c, *tmp, *target = NULL, *sel = focustop(selmon);
	LayoutNode *sel_node, *target_node;
	int closest_dist = INT_MAX, dist, sel_center_x, sel_center_y,
		cand_center_x, cand_center_y;
	int is_vertical, overlaps, best_overlaps = 0;

	if (!sel || sel->isfullscreen ||
		!*get_current_root(selmon) || !selmon->lt[selmon->sellt]->arrange)
		return;

	/* Get the center coordinates of the selected client */
	sel_center_x = sel->geom.x + sel->geom.width / 2;
	sel_center_y = sel->geom.y + sel->geom.height / 2;

	/* Determine if movement is vertical (up/down) or horizontal (left/right) */
	is_vertical = (arg->ui == 2 || arg->ui == 3);

	wl_list_for_each(c, &clients, link) {
		if (!VISIBLEON(c, selmon) || c->isfloating || c->isfullscreen || c == sel)
			continue;

		/* Get the center of candidate client */
		cand_center_x = c->geom.x + c->geom.width / 2;
		cand_center_y = c->geom.y + c->geom.height / 2;

		/* Check that the candidate lies in the requested direction. */
		switch (arg->ui) {
			case 0: /* left */
				if (cand_center_x >= sel_center_x)
					continue;
				break;
			case 1: /* right */
				if (cand_center_x <= sel_center_x)
					continue;
				break;
			case 2: /* up */
				if (cand_center_y >= sel_center_y)
					continue;
				break;
			case 3: /* down */
				if (cand_center_y <= sel_center_y)
					continue;
				break;
			default:
				continue;
		}

		/* Check if candidate overlaps with selected tile in the perpendicular axis */
		if (is_vertical) {
			/* For up/down: check if x-ranges overlap (same column) */
			overlaps = !(c->geom.x + c->geom.width <= sel->geom.x ||
			             c->geom.x >= sel->geom.x + sel->geom.width);
		} else {
			/* For left/right: check if y-ranges overlap (same row) */
			overlaps = !(c->geom.y + c->geom.height <= sel->geom.y ||
			             c->geom.y >= sel->geom.y + sel->geom.height);
		}

		/* Calculate distance in the primary direction only */
		if (is_vertical)
			dist = abs(sel_center_y - cand_center_y);
		else
			dist = abs(sel_center_x - cand_center_x);

		/* Prioritize overlapping tiles, then by distance */
		if (overlaps > best_overlaps ||
		    (overlaps == best_overlaps && dist < closest_dist)) {
			best_overlaps = overlaps;
			closest_dist = dist;
			target = c;
		}
	}

	/* If target is found, swap the two clients' positions in the layout tree */
	if (target) {
		sel_node = find_client_node(*get_current_root(selmon), sel);
		target_node = find_client_node(*get_current_root(selmon), target);
		if (sel_node && target_node) {
			tmp = sel_node->client;
			sel_node->client = target_node->client;
			target_node->client = tmp;
			arrange(selmon);
			/* Keep focus on the original client in its new position */
			focusclient(sel, 1);
		}
	}
}

static void focusdir(const Arg *arg) {
	Client *c, *target = NULL, *sel = focustop(selmon);
	int closest_dist = INT_MAX, dist, sel_center_x, sel_center_y,
		cand_center_x, cand_center_y;
	int is_vertical, overlaps, best_overlaps = 0;

	if (!sel || sel->isfullscreen)
		return;

	/* Get the center coordinates of the selected client */
	sel_center_x = sel->geom.x + sel->geom.width / 2;
	sel_center_y = sel->geom.y + sel->geom.height / 2;

	/* Determine if movement is vertical (up/down) or horizontal (left/right) */
	is_vertical = (arg->ui == DIR_UP || arg->ui == DIR_DOWN);

	wl_list_for_each(c, &clients, link) {
		if (!VISIBLEON(c, selmon) || c->isfloating || c->isfullscreen || c == sel)
			continue;

		/* Get the center of candidate client */
		cand_center_x = c->geom.x + c->geom.width / 2;
		cand_center_y = c->geom.y + c->geom.height / 2;

		/* Check that the candidate lies in the requested direction. */
		switch (arg->ui) {
			case DIR_LEFT:
				if (cand_center_x >= sel_center_x)
					continue;
				break;
			case DIR_RIGHT:
				if (cand_center_x <= sel_center_x)
					continue;
				break;
			case DIR_UP:
				if (cand_center_y >= sel_center_y)
					continue;
				break;
			case DIR_DOWN:
				if (cand_center_y <= sel_center_y)
					continue;
				break;
			default:
				continue;
		}

		/* Check if candidate overlaps with selected tile in the perpendicular axis */
		if (is_vertical) {
			overlaps = !(c->geom.x + c->geom.width <= sel->geom.x ||
			             c->geom.x >= sel->geom.x + sel->geom.width);
		} else {
			overlaps = !(c->geom.y + c->geom.height <= sel->geom.y ||
			             c->geom.y >= sel->geom.y + sel->geom.height);
		}

		/* Calculate distance in the primary direction only */
		if (is_vertical)
			dist = abs(sel_center_y - cand_center_y);
		else
			dist = abs(sel_center_x - cand_center_x);

		/* Prioritize overlapping tiles, then by distance */
		if (overlaps > best_overlaps ||
		    (overlaps == best_overlaps && dist < closest_dist)) {
			best_overlaps = overlaps;
			closest_dist = dist;
			target = c;
		}
	}

	/* If target is found, focus it and warp cursor to its center */
	if (target) {
		focusclient(target, 1);
		wlr_cursor_warp(cursor, NULL,
			target->geom.x + target->geom.width / 2,
			target->geom.y + target->geom.height / 2);
	}
}

static unsigned int
count_columns(LayoutNode *node, Monitor *m)
{
	/* Count columns at the top level only.
	 * A "column" is a subtree rooted at either:
	 * - A client node under vertical splits
	 * - A horizontal split under vertical splits
	 * Once we hit a horizontal split, that's one column (don't recurse). */
	if (!node)
		return 0;
	if (node->is_client_node)
		return 1;
	if (!node->is_split_vertically)
		return 1; /* Horizontal split = 1 column (don't look inside) */

	/* Vertical split at top level - recurse to count columns */
	return count_columns(node->left, m) + count_columns(node->right, m);
}

static int
collect_columns(LayoutNode *node, Monitor *m, LayoutNode **out_nodes,
		unsigned int *out_counts, Client **out_clients, int max_out)
{
	int used = 0;
	unsigned int pc;

	if (!node || max_out <= 0)
		return 0;

	/* Any non-vertical subtree represents a single column. */
	if (node->is_client_node || !node->is_split_vertically) {
		pc = placement_count(node, m);
		if (pc == 0)
			return 0;
		if (out_nodes)
			out_nodes[used] = node;
		if (out_counts)
			out_counts[used] = pc;
		if (out_clients)
			out_clients[used] = first_active_client(node, m);
		return 1;
	}

	used = collect_columns(node->left, m, out_nodes, out_counts, out_clients, max_out);
	if (used < max_out) {
		used += collect_columns(node->right, m,
				out_nodes ? out_nodes + used : NULL,
				out_counts ? out_counts + used : NULL,
				out_clients ? out_clients + used : NULL,
				max_out - used);
	}
	return used;
}

static unsigned int
visible_count(LayoutNode *node, Monitor *m)
{
	Client *c;

	if (!node)
		return 0;
	/* Check if this client is visible. */
	if (node->is_client_node) {
		c = node->client;
		if (!c)
			return 0;
		/* Count dragging_client as visible to preserve its space */
		if (c == dragging_client)
			return 1;
		if (VISIBLEON(c, m) && !c->isfloating && !c->isfullscreen)
			return 1;
		return 0;
	}
	/* Else it's a split node. */
	return visible_count(node->left, m) + visible_count(node->right, m);
}

static unsigned int
placement_count(LayoutNode *node, Monitor *m)
{
	Client *c;

	if (!node)
		return 0;
	if (node->is_client_node) {
		c = node->client;
		if (!c || c->isfullscreen || c->isfloating)
			return 0;
		return VISIBLEON(c, m) ? 1 : 0;
	}
	return placement_count(node->left, m) + placement_count(node->right, m);
}

static unsigned int
target_columns(Monitor *m)
{
	float ratio;

	/* 16:9-ish -> 2 cols, ~21:9 -> 3 cols, ~32:9 -> 4 cols */
	if (!m || m->w.height == 0)
		return 2;

	ratio = (float)m->w.width / (float)m->w.height;
	if (ratio >= 3.2f)
		return 4;
	if (ratio >= 2.2f)
		return 3;
	return 2;
}

static Client *
first_visible_client(LayoutNode *node, Monitor *m)
{
	Client *c;

	if (!node)
		return NULL;
	if (node->is_client_node) {
		c = node->client;
		return (c && VISIBLEON(c, m) && !c->isfloating && !c->isfullscreen) ? c : NULL;
	}

	c = first_visible_client(node->left, m);
	return c ? c : first_visible_client(node->right, m);
}

static Client *
first_active_client(LayoutNode *node, Monitor *m)
{
	Client *c;

	if (!node)
		return NULL;
	if (node->is_client_node) {
		c = node->client;
		if (c && !c->isfullscreen && !c->isfloating && VISIBLEON(c, m))
			return c;
		return NULL;
	}

	c = first_active_client(node->left, m);
	return c ? c : first_active_client(node->right, m);
}

static Client *
largest_client_in_column(LayoutNode *col_node, Monitor *m)
{
	Client *c, *largest = NULL;
	int max_area = 0;

	if (!col_node)
		return NULL;

	if (col_node->is_client_node) {
		c = col_node->client;
		if (c && !c->isfullscreen && !c->isfloating && VISIBLEON(c, m))
			return c;
		return NULL;
	}

	/* Check left subtree first (top/left preference) */
	c = largest_client_in_column(col_node->left, m);
	if (c) {
		int area = c->geom.width * c->geom.height;
		if (area >= max_area) {  /* >= to prefer left/top */
			max_area = area;
			largest = c;
		}
	}
	/* Only pick right if strictly larger */
	c = largest_client_in_column(col_node->right, m);
	if (c) {
		int area = c->geom.width * c->geom.height;
		if (area > max_area) {  /* > to prefer left when equal */
			largest = c;
		}
	}
	return largest;
}

static Client *
pick_target_client(Monitor *m, Client *focused_client)
{
	LayoutNode *col_nodes[64];
	unsigned int col_counts[64];
	Client *col_clients[64];
	int min_cols[64];
	unsigned int min_count = UINT_MAX;
	int ncols, i, min_len = 0;
	LayoutNode **root;

	if (!m)
		return focused_client;

	root = get_current_root(m);
	if (!*root)
		return focused_client;

	ncols = collect_columns(*root, m, col_nodes, col_counts, col_clients, (int)LENGTH(col_nodes));
	for (i = 0; i < ncols; i++) {
		if (col_counts[i] < min_count) {
			min_count = col_counts[i];
			min_len = 0;
			min_cols[min_len++] = i;
		} else if (col_counts[i] == min_count && min_len < (int)LENGTH(min_cols)) {
			min_cols[min_len++] = i;
		}
	}

	if (min_len > 0) {
		int start = col_pick_toggle % min_len;
		int offset;

		for (offset = 0; offset < min_len; offset++) {
			int pick = min_cols[(start + offset) % min_len];
			Client *target = largest_client_in_column(col_nodes[pick], m);
			if (target) {
				col_pick_toggle++;
				return target;
			}
		}
	}

	/* Fall back to any active client (avoids mouse-driven target). */
	return first_active_client(*root, m);
}

static Client *
xytoclient(double x, double y) {
	Client *c, *closest = NULL;
	double dist, mindist = INT_MAX, dx, dy;

	/* Skip dragging_client when finding target */
	wl_list_for_each_reverse(c, &clients, link) {
		if (c == dragging_client)
			continue;
		if (VISIBLEON(c, selmon) && !c->isfloating && !c->isfullscreen &&
			x >= c->geom.x && x <= (c->geom.x + c->geom.width) &&
			y >= c->geom.y && y <= (c->geom.y + c->geom.height)){
			return c;
		}
	}

	/* If no client was found at cursor position fallback to closest. */
	wl_list_for_each_reverse(c, &clients, link) {
		if (c == dragging_client)
			continue;
		if (VISIBLEON(c, selmon) && !c->isfloating && !c->isfullscreen) {
			dx = 0, dy = 0;

			if (x < c->geom.x)
				dx = c->geom.x - x;
			else if (x > (c->geom.x + c->geom.width))
				dx = x - (c->geom.x + c->geom.width);

			if (y < c->geom.y)
				dy = c->geom.y - y;
			else if (y > (c->geom.y + c->geom.height))
				dy = y - (c->geom.y + c->geom.height);

			dist = sqrt(dx * dx + dy * dy);
			if (dist < mindist) {
				mindist = dist;
				closest = c;
			}
		}
	}
	return closest;
}

static LayoutNode *
find_column_root(LayoutNode *node, Monitor *m)
{
	LayoutNode *n, *col_candidate;
	LayoutNode **root;

	if (!node || !m)
		return NULL;

	root = get_current_root(m);
	if (!*root)
		return NULL;

	/* If root is not a vertical split, there's only one "column" (the root) */
	if ((*root)->is_client_node || !(*root)->is_split_vertically)
		return *root;

	/* Walk up to find the node that is a direct child of the root vertical split */
	n = node;
	col_candidate = node;
	while (n->split_node) {
		col_candidate = n;
		n = n->split_node;
	}
	/* col_candidate is now a direct child of root */
	return col_candidate;
}

static unsigned int
count_in_column(LayoutNode *col_node, Monitor *m)
{
	if (!col_node)
		return 0;
	return visible_count(col_node, m);
}

/* Find which half (top/bottom) of a column a node is in.
 * Returns the half subtree node, or NULL if not in a split column. */
static LayoutNode *
find_column_half(LayoutNode *node, Monitor *m)
{
	LayoutNode *col_root;
	LayoutNode *n;

	if (!node || !m)
		return NULL;

	col_root = find_column_root(node, m);
	if (!col_root || col_root->is_client_node)
		return NULL;

	/* Column root should be a horizontal split (top/bottom) */
	if (col_root->is_split_vertically)
		return NULL;

	/* Walk up from node until we hit a direct child of col_root */
	n = node;
	while (n && n->split_node != col_root)
		n = n->split_node;

	return n; /* This is the half (top or bottom) */
}

/* Check if two clients are in the same column */
static int
same_column(Monitor *m, Client *c1, Client *c2)
{
	LayoutNode *node1, *node2, *col1, *col2;
	LayoutNode **root;

	if (!m || !c1 || !c2)
		return 0;

	root = get_current_root(m);
	if (!*root)
		return 0;

	node1 = find_client_node(*root, c1);
	node2 = find_client_node(*root, c2);
	if (!node1 || !node2)
		return 0;

	col1 = find_column_root(node1, m);
	col2 = find_column_root(node2, m);

	return (col1 && col2 && col1 == col2);
}

/* Check if moving a tile from source to target is allowed
 * Returns: 0 = blocked, 1 = normal move, 2 = swap in same column */
static int
can_move_tile(Monitor *m, Client *source, Client *target)
{
	LayoutNode *source_node, *target_node, *source_col, *target_col;
	LayoutNode **root;
	unsigned int source_col_tiles, target_col_tiles;

	if (!m || !source)
		return 0;

	root = get_current_root(m);
	if (!*root)
		return 1; /* No tree yet, allow */

	source_node = find_client_node(*root, source);
	if (!source_node)
		return 1; /* Source not in tree, allow */

	source_col = find_column_root(source_node, m);
	source_col_tiles = count_in_column(source_col, m);

	if (!target)
		return 1; /* No target, normal insert */

	target_node = find_client_node(*root, target);
	if (!target_node)
		return 1; /* Target not in tree, allow */

	target_col = find_column_root(target_node, m);
	target_col_tiles = count_in_column(target_col, m);

	/* Check if same column */
	if (source_col && target_col && source_col == target_col) {
		/* Same column with 4 tiles: block all LOCAL movement */
		if (source_col_tiles >= 4)
			return 0;

		/* Swap only if:
		 * 1. Both tiles are siblings (same parent split), OR
		 * 2. Source is a direct child of column root (big tile) */
		if (source_node->split_node && source_node->split_node == target_node->split_node) {
			/* Siblings - swap them */
			return 2;
		}
		if (source_col && !source_col->is_client_node &&
		    source_node->split_node == source_col) {
			/* Source is direct child of column root - swap halves */
			return 2;
		}

		/* Other same-column cases: allow normal move (e.g., small over big) */
		return 1;
	}

	/* Different column: check if target column is full */
	if (target_col_tiles >= 4)
		return 0;

	/* Check if target half (top/bottom) is full (max 2 tiles per half) */
	if (target_col_tiles >= 2) {
		LayoutNode *target_half = find_column_half(target_node, m);
		if (target_half && visible_count(target_half, m) >= 2)
			return 0;
	}

	/* Moving to different column is allowed (even from a 4-tile column) */
	return 1;
}

/* Swap two tiles' positions within the same column.
 * - If source is a direct child of column root: swap subtrees (big tile ↔ group of small tiles)
 * - If both are siblings (same parent): swap children of their parent
 * - Otherwise: just swap client pointers */
static void
swap_tiles_in_tree(Monitor *m, Client *c1, Client *c2)
{
	LayoutNode *node1, *node2, *col_root;
	LayoutNode **root;
	LayoutNode *tmp_node;
	Client *tmp_client;

	if (!m || !c1 || !c2 || c1 == c2)
		return;

	root = get_current_root(m);
	if (!*root)
		return;

	node1 = find_client_node(*root, c1);
	node2 = find_client_node(*root, c2);
	if (!node1 || !node2)
		return;

	col_root = find_column_root(node1, m);
	if (!col_root)
		return;

	/* If col_root is a client node, no swapping possible */
	if (col_root->is_client_node)
		return;

	/* Check if both tiles are siblings (share the same parent split) */
	if (node1->split_node && node1->split_node == node2->split_node) {
		/* Siblings - swap children of their parent */
		LayoutNode *parent = node1->split_node;
		if (parent->left && parent->right) {
			tmp_node = parent->left;
			parent->left = parent->right;
			parent->right = tmp_node;
		}
		return;
	}

	/* Check if source (node1) is a direct child of the column root */
	if (node1->split_node == col_root) {
		/* Source is at top level of column - swap the two halves of the column */
		if (col_root->left && col_root->right) {
			tmp_node = col_root->left;
			col_root->left = col_root->right;
			col_root->right = tmp_node;
		}
	} else {
		/* Source is deeper in the structure - just swap client pointers */
		tmp_client = node1->client;
		node1->client = node2->client;
		node2->client = tmp_client;
	}
}

static void
start_tile_drag(Monitor *m, Client *c)
{
	LayoutNode *client_node, *col_root;
	LayoutNode **root;

	if (!m || !c)
		return;

	root = get_current_root(m);
	if (!*root)
		return;

	client_node = find_client_node(*root, c);
	if (!client_node)
		return;

	dragging_client = c;
	drag_source_node = client_node;
	drag_placeholder_box = c->geom;

	/* Find the column this client belongs to and count tiles in it */
	col_root = find_column_root(client_node, m);
	drag_was_alone_in_column = (count_in_column(col_root, m) == 1);
}

static void
end_tile_drag(void)
{
	dragging_client = NULL;
	drag_source_node = NULL;
	drag_was_alone_in_column = 0;
	drag_placeholder_box = (struct wlr_box){0};
}

static void
swap_columns(Monitor *m, Client *c1, Client *c2)
{
	LayoutNode *node1, *node2, *col1, *col2;
	LayoutNode *tmp_left, *tmp_right;
	LayoutNode **root;

	if (!m || !c1 || !c2)
		return;

	root = get_current_root(m);
	if (!*root)
		return;

	node1 = find_client_node(*root, c1);
	node2 = find_client_node(*root, c2);
	if (!node1 || !node2)
		return;

	col1 = find_column_root(node1, m);
	col2 = find_column_root(node2, m);
	if (!col1 || !col2 || col1 == col2)
		return;

	/* If both columns share the same parent split node, swap them */
	if (col1->split_node && col1->split_node == col2->split_node) {
		LayoutNode *parent = col1->split_node;
		tmp_left = parent->left;
		tmp_right = parent->right;
		parent->left = tmp_right;
		parent->right = tmp_left;
	}
}
