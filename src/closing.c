/* Closed tiles fade where they sat. */
#include "nixlytile.h"

/* Fades out in ~200 ms. */
static const SpringParams SPRING_CLOSE = { 1.0, 1.0, 900.0 };

static void
closing_free(ClosingAnim *a)
{
	wl_list_remove(&a->link);
	snapshot_destroy(a->snap);
	free(a);
}

void
anim_spawn_close(Client *c)
{
	ClosingAnim *a;
	Monitor *m = c->mon;
	struct nx_box at = c->geom;

	if (!m || !c->scene || !c->scene->node.enabled || !c->column ||
			at.width <= 0 || at.height <= 0)
		return;
	a = ecalloc(1, sizeof(*a));
	a->snap = snapshot_take(layers[LyrTile], c->scene,
			(struct nx_box){ 0, 0, at.width, at.height });
	if (!a->snap) {
		free(a);
		return;
	}
	/* Neighbours slide over it. */
	nx_scene_node_lower_to_bottom(snapshot_node(a->snap));
	snapshot_fit(a->snap, at, m->w);
	a->mon = m;
	a->ws_id = c->column->ws->window_id;
	a->spot = (struct nx_box){ at.x - c->tile_ox, at.y - c->tile_oy,
			at.width, at.height };
	a->progress = PROGRESS_SPAN;
	wl_list_insert(&closing_anims, &a->link);
	nx_output_schedule_frame(m->nx_output);
}

/* Ghosts ride their workspace's camera. */
void
closing_anims_place(Workspace *ws, int ox, int oy)
{
	ClosingAnim *a;
	struct nx_box at;

	wl_list_for_each(a, &closing_anims, link) {
		if (a->ws_id != ws->window_id)
			continue;
		at = a->spot;
		at.x += ox;
		at.y += oy;
		snapshot_fit(a->snap, at, a->mon->w);
	}
}

void
closing_anims_tick(Monitor *m, double dt, int *still)
{
	ClosingAnim *a, *tmp;

	*still = 0;
	wl_list_for_each_safe(a, tmp, &closing_anims, link) {
		if (a->mon != m)
			continue;
		if (!spring_tick(&a->progress, &a->vel, 0.0, SPRING_CLOSE, dt) ||
				a->progress <= 0.0) {
			closing_free(a);
			continue;
		}
		snapshot_set_alpha(a->snap, (float)(a->progress / PROGRESS_SPAN));
		*still = 1;
	}
}

void
closing_anims_drop(Monitor *m)
{
	ClosingAnim *a, *tmp;

	wl_list_for_each_safe(a, tmp, &closing_anims, link)
		if (a->mon == m)
			closing_free(a);
}
