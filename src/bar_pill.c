#include "nixlytile.h"

#define FADE_RATE 22.0
#define FADE_EPSILON 0.004

static const SpringParams SPRING_PILL = { 1.0, 1.0, 1400.0 };
static const float clear[4];

static int
fade_tick(double *a, double target, double dt)
{
	if (*a == target)
		return 0;
	*a += (target - *a) * (1.0 - exp(-FADE_RATE * dt));
	if (fabs(target - *a) < FADE_EPSILON)
		*a = target;
	return 1;
}

/* Height-dependent child geometry. */
static void
place_children(BarPill *p)
{
	int h = p->cap_h, r = h / 2;

	nx_scene_buffer_set_source_box(p->cap_l, &(struct nx_fbox){ 0, 0, r, h });
	nx_scene_buffer_set_dest_size(p->cap_l, r, h);
	nx_scene_buffer_set_source_box(p->cap_r, &(struct nx_fbox){ h - r, 0, r, h });
	nx_scene_buffer_set_dest_size(p->cap_r, r, h);
	nx_scene_node_set_position(&p->mid->node, r, 0);
	if (!p->edge_t)
		return;
	nx_scene_node_set_position(&p->edge_t->node, r, 0);
	nx_scene_node_set_position(&p->edge_b->node, r, h - 1);
}

static void
pill_set_caps(BarPill *p, int h)
{
	struct nx_buffer *disc;

	if (p->cap_h == h)
		return;
	disc = bar_disc_buffer(h, p->fill, p->edge);
	if (!disc)
		return;
	nx_scene_buffer_set_buffer(p->cap_l, disc);
	nx_scene_buffer_set_buffer(p->cap_r, disc);
	nx_buffer_drop(disc);
	p->cap_h = h;
	place_children(p);
}

static void
apply_edges(BarPill *p, int span, float a)
{
	float col[4];

	if (!p->edge_t)
		return;
	bar_color_premul(p->edge, a, col);
	nx_scene_rect_set_size(p->edge_t, span, 1);
	nx_scene_rect_set_size(p->edge_b, span, 1);
	nx_scene_rect_set_color(p->edge_t, col);
	nx_scene_rect_set_color(p->edge_b, col);
}

/* Tree carries position; children stay relative. */
static void
pill_apply(BarPill *p)
{
	float col[4];
	int h = p->cap_h, r = h / 2;
	int w = MAX((int)lround(p->w), h);
	float a = (float)MAX(0.0, MIN(p->a, 1.0));

	nx_scene_node_set_enabled(&p->tree->node, a > 0.0f && h > 0);
	if (a <= 0.0f || h <= 0)
		return;
	nx_scene_node_set_position(&p->tree->node, (int)lround(p->x), p->y);
	nx_scene_buffer_set_opacity(p->cap_l, a);
	nx_scene_buffer_set_opacity(p->cap_r, a);
	nx_scene_node_set_position(&p->cap_r->node, w - r, 0);
	nx_scene_rect_set_size(p->mid, w - 2 * r, h);
	bar_color_premul(p->fill, a, col);
	nx_scene_rect_set_color(p->mid, col);
	apply_edges(p, w - 2 * r, a);
}

static void
pill_init(BarPill *p, struct nx_scene_tree *parent)
{
	p->tree = nx_scene_tree_create(parent);
	p->cap_l = nx_scene_buffer_create(p->tree, NULL);
	p->mid = nx_scene_rect_create(p->tree, 0, 0, clear);
	if (p->edge[3] > 0.0f) {
		p->edge_t = nx_scene_rect_create(p->tree, 0, 0, clear);
		p->edge_b = nx_scene_rect_create(p->tree, 0, 0, clear);
	}
	p->cap_r = nx_scene_buffer_create(p->tree, NULL);
	nx_scene_node_set_enabled(&p->tree->node, 0);
}

/* Islands under modules; ws pill under its digits. */
void
bar_pills_init(StatusBar *bar)
{
	struct nx_scene_tree *under = nx_scene_tree_create(bar->tree);

	nx_scene_node_lower_to_bottom(&under->node);

	for (int i = 0; i < PILL_HOVER; i++)
		bar->pill[i] = (BarPill){ .fill = statusbar_island_bg,
			.edge = statusbar_island_edge };
	bar->pill[PILL_HOVER] = (BarPill){ .fill = statusbar_hover_bg, .edge = clear };
	bar->pill[PILL_WS] = (BarPill){ .fill = statusbar_ws_active_bg, .edge = clear };
	for (int i = 0; i < PILL_WS; i++)
		pill_init(&bar->pill[i], under);
	pill_init(&bar->pill[PILL_WS], bar->tags.tree);
	nx_scene_node_lower_to_bottom(&bar->pill[PILL_WS].tree->node);
}

/* Retarget; 1 when anything changed. */
int
bar_pill_target(BarPill *p, Monitor *m, const BarSpan *s)
{
	if (!p->tree || (p->tx == s->x && p->tw == s->w && p->y == s->y
			&& p->cap_h == s->h && p->ta == 1.0))
		return 0;
	if (p->a <= 0.0 || p->cap_h != s->h) {
		p->x = s->x;
		p->w = s->w;
		p->vx = p->vw = 0.0;
	}
	pill_set_caps(p, s->h);
	p->y = s->y;
	p->tx = s->x;
	p->tw = s->w;
	p->ta = 1.0;
	pill_apply(p);
	if (m->nx_output)
		nx_output_schedule_frame(m->nx_output);
	return 1;
}

void
bar_pill_hide(BarPill *p, Monitor *m)
{
	if (!p->tree || p->ta == 0.0)
		return;
	p->ta = 0.0;
	if (m->nx_output)
		nx_output_schedule_frame(m->nx_output);
}

/* Jump to target, no motion. */
void
bar_pill_settle(BarPill *p)
{
	if (!p->tree)
		return;
	p->x = p->tx;
	p->w = p->tw;
	p->a = p->ta;
	p->vx = p->vw = 0.0;
	pill_apply(p);
}

/* Caps re-raster on next target. */
void
bar_pills_invalidate(StatusBar *bar)
{
	for (int i = 0; i < BAR_PILLS; i++)
		bar->pill[i].cap_h = 0;
}

static int
pill_tick(BarPill *p, double dt)
{
	int moving = 0;

	if (!p->tree)
		return 0;
	moving |= spring_tick(&p->x, &p->vx, p->tx, SPRING_PILL, dt);
	moving |= spring_tick(&p->w, &p->vw, p->tw, SPRING_PILL, dt);
	moving |= fade_tick(&p->a, p->ta, dt);
	if (moving)
		pill_apply(p);
	return moving;
}

int
statusbar_anim_tick(Monitor *m, double dt)
{
	int moving = 0;

	for (int i = 0; i < BAR_PILLS; i++)
		moving |= pill_tick(&m->statusbar.pill[i], dt);
	return moving;
}
