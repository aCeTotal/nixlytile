/* Resizes show crisp, crossfaded content. */
#include "nixlytile.h"
#include "client.h"

/* Crossfade settles in ~150 ms. */
static const SpringParams SPRING_MORPH = { 1.0, 1.0, 1400.0 };
/* Settled content may still lag. */
#define SETTLE_GRACE_NS 300000000ULL
/* Floating and fullscreen never crop. */
static const struct nx_box UNBOUNDED = { -(1 << 20), -(1 << 20), 1 << 21, 1 << 21 };

static int
active(Client *c)
{
	return c->morph_live != NULL;
}

/* Content box, client tree coords. */
static struct nx_box
inner(Client *c)
{
	int bw = (int)c->bw;

	return (struct nx_box){ bw, bw, MAX(c->geom.width - 2 * bw, 1),
			MAX(c->geom.height - 2 * bw, 1) };
}

static struct nx_box
visible_area(Client *c)
{
	struct nx_box a;

	if (!c->column || c->isfloating || c->isfullscreen || !c->mon)
		return UNBOUNDED;
	a = c->mon->w;
	a.x -= c->geom.x;
	a.y -= c->geom.y;
	return a;
}

/* The live surface cropped to w×h. */
static Snapshot *
capture(Client *c, int w, int h)
{
	struct nx_box clip;

	client_get_clip(c, &clip);
	clip.width = w;
	clip.height = h;
	nx_scene_subsurface_tree_set_clip(&c->scene_surface->node, &clip);
	return snapshot_take(c->scene, c->scene_surface,
			(struct nx_box){ 0, 0, w, h });
}

/* What the tile shows right now. */
static Snapshot *
copy_shown(Client *c)
{
	struct nx_box box = inner(c);

	if (active(c))
		return snapshot_take(c->scene,
				nx_scene_tree_from_node(snapshot_node(c->morph_live)), box);
	return capture(c, box.width, box.height);
}

static int
begin(Client *c)
{
	struct nx_surface *s = client_surface(c);
	int cw, ch;

	if (active(c))
		return 1;
	if (!s || !s->mapped || !s->buffer || !c->scene_surface)
		return 0;
	client_get_committed_size(c, &cw, &ch);
	if (cw <= 0 || ch <= 0)
		return 0;
	client_unfreeze(c);
	c->morph_live = capture(c, cw, ch);
	if (!c->morph_live)
		return 0;
	nx_scene_tree_set_opacity(c->scene_surface, 0.0f);
	c->morph_settled_ns = 0;
	return 1;
}

/* Old content stays until answered. */
void
morph_expect(Client *c)
{
	struct nx_surface *s = client_surface(c);

	if (!s || !s->mapped || !s->buffer || !c->scene_surface)
		return;
	if (!c->morph_from) {
		c->morph_from = copy_shown(c);
		c->morph_fade = c->morph_fade_v = 0.0;
		c->morph_stretch = 0;
	}
	if (!begin(c)) {
		snapshot_destroy(c->morph_from);
		c->morph_from = NULL;
		return;
	}
	c->morph_serial = c->txn_serial;
	c->morph_w = c->txn_w;
	c->morph_h = c->txn_h;
	c->morph_matched = c->morph_answered = 0;
	c->morph_expect_ns = get_time_ns();
	morph_fit(c);
}

/* Live content only, for drags. */
void
morph_track(Client *c)
{
	if (begin(c))
		morph_fit(c);
}

static void
stack(Client *c, Snapshot *bottom, Snapshot *top)
{
	nx_scene_node_set_enabled(snapshot_node(bottom), 1);
	nx_scene_node_set_enabled(snapshot_node(top), 1);
	nx_scene_node_place_above(snapshot_node(bottom), &c->scene_surface->node);
	nx_scene_node_place_above(snapshot_node(top), snapshot_node(bottom));
}

/* Translucent layers never overlap. */
static void
show_only(Client *c, Snapshot *shown, Snapshot *hidden)
{
	nx_scene_node_set_enabled(snapshot_node(hidden), 0);
	nx_scene_node_set_enabled(snapshot_node(shown), 1);
	nx_scene_node_place_above(snapshot_node(shown), &c->scene_surface->node);
	snapshot_fit_cover(shown, inner(c), visible_area(c));
	snapshot_set_alpha(shown, 1.0f);
}

void
morph_fit(Client *c)
{
	Snapshot *live = c->morph_live, *from = c->morph_from;
	struct nx_box box, area;
	int live_covers, from_covers;
	float t;

	if (!active(c))
		return;
	box = inner(c);
	area = visible_area(c);
	if (!from) {
		nx_scene_node_set_enabled(snapshot_node(live), 1);
		nx_scene_node_place_above(snapshot_node(live), &c->scene_surface->node);
		snapshot_fit_cover(live, box, area);
		return;
	}
	if (!snapshot_opaque(live) || !snapshot_opaque(from)) {
		if (c->morph_fade > 0.0)
			show_only(c, live, from);
		else
			show_only(c, from, live);
		return;
	}
	t = (float)(c->morph_fade / PROGRESS_SPAN);
	live_covers = snapshot_covers(live, box);
	from_covers = snapshot_covers(from, box);
	/* Content lags: stretch, once. */
	c->morph_stretch |= !live_covers && !from_covers;
	if (!live_covers && from_covers) {
		/* Shrink: new fades in. */
		stack(c, from, live);
		snapshot_fit_crisp(from, box, area);
		snapshot_fit_crisp(live, box, area);
		snapshot_set_alpha(from, 1.0f);
		snapshot_set_alpha(live, t);
		return;
	}
	/* Grow: old fades out. */
	stack(c, live, from);
	snapshot_fit_cover(live, box, area);
	(c->morph_stretch ? snapshot_fit_cover : snapshot_fit_crisp)(from, box, area);
	snapshot_set_alpha(live, 1.0f);
	snapshot_set_alpha(from, 1.0f - t);
}

/* Runs on applied commits. */
void
morph_commit(Client *c)
{
	struct nx_surface *s = client_surface(c);
	Snapshot *live;
	int cw, ch;

	if (!active(c) || !s->buffer ||
			!(s->current.committed & NX_SURFACE_STATE_BUFFER))
		return;
	client_get_committed_size(c, &cw, &ch);
	if (cw <= 0 || ch <= 0)
		return;
	live = capture(c, cw, ch);
	if (!live)
		return;
	snapshot_destroy(c->morph_live);
	c->morph_live = live;
	answer_note(c);
	morph_fit(c);
}

void
morph_end(Client *c)
{
	struct nx_box clip;

	if (!active(c))
		return;
	snapshot_destroy(c->morph_from);
	snapshot_destroy(c->morph_live);
	c->morph_from = c->morph_live = NULL;
	nx_scene_tree_set_opacity(c->scene_surface, 1.0f);
	client_get_clip(c, &clip);
	nx_scene_subsurface_tree_set_clip(&c->scene_surface->node, &clip);
	client_clip_to_usable(c);
	converge_kick(c);
}

static int
box_moving(Client *c)
{
	return c->mon->tile_spring || c->fsx_active || c->anim_active ||
			live_resize_active();
}

static void
fade(Client *c, double dt)
{
	if (c->morph_fade < PROGRESS_SPAN && !spring_tick(&c->morph_fade,
			&c->morph_fade_v, PROGRESS_SPAN, SPRING_MORPH, dt))
		c->morph_fade = PROGRESS_SPAN;
	/* Old content goes once hidden. */
	if (c->morph_fade >= PROGRESS_SPAN &&
			(snapshot_covers(c->morph_live, inner(c)) || !box_moving(c))) {
		snapshot_destroy(c->morph_from);
		c->morph_from = NULL;
	}
	morph_fit(c);
}

static int
fits(Client *c)
{
	struct nx_box box = inner(c);
	int cw, ch;

	client_get_committed_size(c, &cw, &ch);
	return abs(cw - box.width) <= SIZE_SLACK_PX &&
			abs(ch - box.height) <= SIZE_SLACK_PX;
}

/* Nonzero while frames are needed. */
static int
step(Client *c, double dt, uint64_t now)
{
	if (c->morph_from && answer_settled(c, now))
		fade(c, dt);
	if (c->morph_from)
		return 1;
	if (box_moving(c)) {
		c->morph_settled_ns = 0;
		return 1;
	}
	if (!c->morph_settled_ns)
		c->morph_settled_ns = now;
	if (!fits(c) && now - c->morph_settled_ns < SETTLE_GRACE_NS)
		return 1;
	morph_end(c);
	return 0;
}

int
morph_tick(Monitor *m, double dt)
{
	Client *c, *tmp;
	uint64_t now = get_time_ns();
	int busy = 0;

	wl_list_for_each_safe(c, tmp, &clients, link)
		if (c->mon == m && active(c))
			busy |= step(c, dt, now);
	return busy;
}
