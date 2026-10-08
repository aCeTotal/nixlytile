#include "nixlytile.h"
#include "client.h"

/* Varsel som glir ut etter unmap. */

typedef struct {
	struct wl_list link;
	Monitor *m;
	struct wlr_scene_tree *tree;
	struct wlr_scene_buffer *sb;
	int bx, by;
	int w, h;
	int buf_w, buf_h;
	int slot_y, off_x;
	double x_f, x_vel;
} NotifGhost;

typedef struct {
	struct wlr_surface *surf;
	int sx, sy;
	int found;
} GhostFind;

static const SpringParams SPRING_GHOST = { 1.0, 1.0, 800.0 };

static struct wl_list ghosts = { &ghosts, &ghosts };

static void
ghost_find_iter(struct wlr_scene_buffer *sb, int sx, int sy, void *data)
{
	GhostFind *f = data;
	struct wlr_scene_surface *ss = wlr_scene_surface_try_from_buffer(sb);

	if (f->found || !ss || ss->surface != f->surf)
		return;
	f->sx = sx;
	f->sy = sy;
	f->found = 1;
}

static void
ghost_clip(NotifGhost *g)
{
	int vis = g->m->m.x + g->m->m.width - ((int)g->x_f + g->bx);

	if (vis <= 0) {
		wlr_scene_node_set_enabled(&g->sb->node, 0);
		return;
	}
	wlr_scene_node_set_enabled(&g->sb->node, 1);
	vis = MIN(vis, g->w);
	wlr_scene_buffer_set_source_box(g->sb, &(struct wlr_fbox){
		0, 0, (double)vis * g->buf_w / g->w, g->buf_h });
	wlr_scene_buffer_set_dest_size(g->sb, vis, g->h);
}

static void
ghost_destroy(NotifGhost *g)
{
	wlr_scene_node_destroy(&g->tree->node);
	wl_list_remove(&g->link);
	free(g);
}

void
notify_ghost_spawn(const Notif *n)
{
	struct wlr_surface *surf = client_surface(n->c);
	GhostFind f = { .surf = surf };
	NotifGhost *g;

	if (!surf || !surf->buffer || !n->c->scene)
		return;
	wlr_scene_node_for_each_buffer(&n->c->scene->node, ghost_find_iter, &f);
	if (!f.found)
		return;
	g = ecalloc(1, sizeof(*g));
	g->tree = wlr_scene_tree_create(layers[LyrOverlay]);
	if (!g->tree) {
		free(g);
		return;
	}
	g->sb = wlr_scene_buffer_create(g->tree, &surf->buffer->base);
	if (!g->sb) {
		wlr_scene_node_destroy(&g->tree->node);
		free(g);
		return;
	}
	g->m = n->m;
	g->bx = f.sx;
	g->by = f.sy;
	g->w = surf->current.width;
	g->h = surf->current.height;
	g->buf_w = surf->buffer->base.width;
	g->buf_h = surf->buffer->base.height;
	g->slot_y = n->slot_y;
	g->off_x = n->off_x;
	g->x_f = n->x_f;
	g->x_vel = n->x_vel;
	wlr_scene_node_set_position(&g->sb->node, g->bx, g->by);
	wlr_scene_node_set_position(&g->tree->node, (int)g->x_f, g->slot_y);
	ghost_clip(g);
	wl_list_insert(&ghosts, &g->link);
	wlr_output_schedule_frame(g->m->wlr_output);
}

void
notify_ghost_tick(Monitor *m, double dt, int *still)
{
	NotifGhost *g, *tmp;

	*still = 0;
	wl_list_for_each_safe(g, tmp, &ghosts, link) {
		if (g->m != m)
			continue;
		if (!spring_tick(&g->x_f, &g->x_vel, (double)g->off_x,
				SPRING_GHOST, dt)) {
			ghost_destroy(g);
			continue;
		}
		wlr_scene_node_set_position(&g->tree->node, (int)g->x_f,
				g->slot_y);
		ghost_clip(g);
		*still = 1;
	}
}

void
notify_ghost_purge_mon(Monitor *m)
{
	NotifGhost *g, *tmp;

	wl_list_for_each_safe(g, tmp, &ghosts, link)
		if (g->m == m)
			ghost_destroy(g);
}
