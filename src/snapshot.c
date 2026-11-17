/* Frozen copy of displayed scene content. */
#include "nixlytile.h"

typedef struct {
	struct nx_scene_node *node;
	struct nx_box rect;
	struct nx_fbox src;
	int opaque;
} Part;

struct Snapshot {
	struct nx_scene_tree *tree;
	int w, h;
	int n, cap;
	Part *part;
};

typedef struct {
	int x, y;
	float alpha;
} Pen;

/* Most windows are one buffer. */
#define FIRST_PARTS 4

static void
add(Snapshot *s, Part p)
{
	if (s->n < s->cap) {
		s->part[s->n++] = p;
		return;
	}
	s->cap = s->cap ? 2 * s->cap : FIRST_PARTS;
	s->part = realloc(s->part, (size_t)s->cap * sizeof(*s->part));
	if (!s->part)
		die("snapshot: out of memory");
	s->part[s->n++] = p;
}

/* Input goes to the live surface. */
static bool
rejects_input(struct nx_scene_buffer *buffer, double *sx, double *sy)
{
	return false;
}

static int
fully_opaque(struct nx_scene_buffer *b, int w, int h)
{
	pixman_box32_t *e = pixman_region32_extents(&b->opaque_region);

	return pixman_region32_n_rects(&b->opaque_region) == 1 &&
			e->x1 <= 0 && e->y1 <= 0 && e->x2 >= w && e->y2 >= h;
}

static void
copy_buffer(Snapshot *s, struct nx_scene_buffer *b, Pen pen)
{
	struct nx_scene_buffer *clone;
	Part p;

	if (!b->buffer)
		return;
	p.src = b->src_box;
	if (nx_fbox_empty(&p.src))
		p.src = (struct nx_fbox){ 0, 0, b->buffer->width, b->buffer->height };
	p.rect = (struct nx_box){ pen.x, pen.y, b->dst_width, b->dst_height };
	if (!p.rect.width || !p.rect.height) {
		p.rect.width = (int)p.src.width;
		p.rect.height = (int)p.src.height;
	}
	if (p.rect.width <= 0 || p.rect.height <= 0)
		return;
	clone = nx_scene_buffer_create(s->tree, b->buffer);
	if (!clone)
		return;
	clone->point_accepts_input = rejects_input;
	nx_scene_buffer_set_transform(clone, b->transform);
	nx_scene_buffer_set_opacity(clone, b->opacity * pen.alpha);
	nx_scene_buffer_set_transfer_function(clone, b->transfer_function);
	nx_scene_buffer_set_primaries(clone, b->primaries);
	nx_scene_buffer_set_color_encoding(clone, b->color_encoding);
	nx_scene_buffer_set_color_range(clone, b->color_range);
	p.node = &clone->node;
	p.opaque = pen.alpha == 1.0f && fully_opaque(b, p.rect.width, p.rect.height);
	add(s, p);
}

static void
copy_rect(Snapshot *s, struct nx_scene_rect *r, Pen pen)
{
	struct nx_scene_rect *clone;
	float color[4];
	int i;

	for (i = 0; i < 4; i++)
		color[i] = r->color[i] * pen.alpha;
	clone = nx_scene_rect_create(s->tree, r->width, r->height, color);
	if (!clone)
		return;
	add(s, (Part){ .node = &clone->node,
			.rect = { pen.x, pen.y, r->width, r->height } });
}

static void
walk(Snapshot *s, struct nx_scene_tree *tree, Pen pen)
{
	struct nx_scene_node *node;
	Pen at;

	wl_list_for_each(node, &tree->children, link) {
		if (!node->enabled)
			continue;
		at = (Pen){ pen.x + node->x, pen.y + node->y, pen.alpha };
		if (node->type == NX_SCENE_NODE_RECT) {
			copy_rect(s, nx_scene_rect_from_node(node), at);
			continue;
		}
		if (node->type == NX_SCENE_NODE_BUFFER) {
			copy_buffer(s, nx_scene_buffer_from_node(node), at);
			continue;
		}
		at.alpha *= nx_scene_tree_from_node(node)->opacity;
		if (at.alpha > 0.0f)
			walk(s, nx_scene_tree_from_node(node), at);
	}
}

/* Copies src at its natural box. */
Snapshot *
snapshot_take(struct nx_scene_tree *parent, struct nx_scene_tree *src,
		struct nx_box natural)
{
	Snapshot *s = ecalloc(1, sizeof(*s));

	s->tree = nx_scene_tree_create(parent);
	if (!s->tree) {
		free(s);
		return NULL;
	}
	s->w = MAX(natural.width, 1);
	s->h = MAX(natural.height, 1);
	walk(s, src, (Pen){ -natural.x, -natural.y, 1.0f });
	return s;
}

static int
edge(int origin, int v, double scale)
{
	return origin + (int)lround(v * scale);
}

static void
place(Part *p, struct nx_box r, struct nx_box v)
{
	struct nx_scene_buffer *b;
	struct nx_fbox src;
	pixman_region32_t opaque;

	nx_scene_node_set_position(p->node, v.x, v.y);
	if (p->node->type == NX_SCENE_NODE_RECT) {
		nx_scene_rect_set_size(nx_scene_rect_from_node(p->node), v.width, v.height);
		return;
	}
	b = nx_scene_buffer_from_node(p->node);
	src = p->src;
	if (b->transform == WL_OUTPUT_TRANSFORM_NORMAL) {
		src.x += p->src.width * (v.x - r.x) / r.width;
		src.y += p->src.height * (v.y - r.y) / r.height;
		src.width = p->src.width * v.width / r.width;
		src.height = p->src.height * v.height / r.height;
	}
	nx_scene_buffer_set_source_box(b, &src);
	nx_scene_buffer_set_dest_size(b, v.width, v.height);
	pixman_region32_init_rect(&opaque, 0, 0, p->opaque ? v.width : 0,
			p->opaque ? v.height : 0);
	nx_scene_buffer_set_opaque_region(b, &opaque);
	pixman_region32_fini(&opaque);
}

/* Stretches into box, crops to area. */
void
snapshot_fit(Snapshot *s, struct nx_box box, struct nx_box area)
{
	double sx = (double)box.width / s->w;
	double sy = (double)box.height / s->h;
	struct nx_box r, v;
	int i;

	for (i = 0; i < s->n; i++) {
		Part *p = &s->part[i];

		r.x = edge(box.x, p->rect.x, sx);
		r.y = edge(box.y, p->rect.y, sy);
		r.width = edge(box.x, p->rect.x + p->rect.width, sx) - r.x;
		r.height = edge(box.y, p->rect.y + p->rect.height, sy) - r.y;
		if (r.width <= 0 || r.height <= 0 ||
				!nx_box_intersection(&v, &r, &area)) {
			nx_scene_node_set_enabled(p->node, 0);
			continue;
		}
		nx_scene_node_set_enabled(p->node, 1);
		place(p, r, v);
	}
}

void
snapshot_set_alpha(Snapshot *s, float alpha)
{
	nx_scene_tree_set_opacity(s->tree, alpha);
}

struct nx_scene_node *
snapshot_node(Snapshot *s)
{
	return &s->tree->node;
}

void
snapshot_destroy(Snapshot *s)
{
	if (!s)
		return;
	nx_scene_node_destroy(&s->tree->node);
	free(s->part);
	free(s);
}

int
snapshot_covers(Snapshot *s, struct nx_box box)
{
	return s->w >= box.width && s->h >= box.height;
}

/* Natural size, cropped to the box. */
void
snapshot_fit_crisp(Snapshot *s, struct nx_box box, struct nx_box area)
{
	struct nx_box clip = { 0 };

	nx_box_intersection(&clip, &box, &area);
	snapshot_fit(s, (struct nx_box){ box.x, box.y, s->w, s->h }, clip);
}

/* Crops overflow, stretches shortfall. */
void
snapshot_fit_cover(Snapshot *s, struct nx_box box, struct nx_box area)
{
	struct nx_box clip = { 0 };

	nx_box_intersection(&clip, &box, &area);
	snapshot_fit(s, (struct nx_box){ box.x, box.y, MAX(s->w, box.width),
			MAX(s->h, box.height) }, clip);
}
