#include "nixlytile.h"
#include "client.h"

static void
strip(struct wlr_scene_rect *r, struct wlr_box b, struct wlr_box area)
{
	struct wlr_box vis;

	if (!wlr_box_intersection(&vis, &b, &area)) {
		wlr_scene_node_set_enabled(&r->node, 0);
		return;
	}
	wlr_scene_node_set_position(&r->node, vis.x, vis.y);
	wlr_scene_rect_set_size(r, vis.width, vis.height);
	wlr_scene_node_set_enabled(&r->node, 1);
}

void
tilefill_create(Client *c)
{
	int i;

	for (i = 0; i < 2; i++) {
		c->fill[i] = wlr_scene_rect_create(c->scene, 0, 0, rootcolor);
		c->fill[i]->node.data = c;
		wlr_scene_node_set_enabled(&c->fill[i]->node, 0);
		wlr_scene_node_lower_to_bottom(&c->fill[i]->node);
	}
}

/* Covers tile area content lacks. */
void
tilefill_update(Client *c)
{
	int bw = (int)c->bw;
	int iw = c->geom.width - 2 * bw;
	int ih = c->geom.height - 2 * bw;
	int frozen = c->frozen_buffer && c->frozen_buffer->node.enabled;
	int nw, nh;
	struct wlr_box area = c->mon->w;

	if (!c->fill[0])
		return;
	client_get_committed_size(c, &nw, &nh);
	if (!c->column || c->isfloating || c->isfullscreen || frozen ||
			nw <= 0 || nh <= 0) {
		nw = iw;
		nh = ih;
	}
	nw = MIN(nw, iw);
	nh = MIN(nh, ih);
	area.x -= c->geom.x;
	area.y -= c->geom.y;
	strip(c->fill[0], (struct wlr_box){ bw + nw, bw, iw - nw, ih }, area);
	strip(c->fill[1], (struct wlr_box){ bw, bw + nh, nw, ih - nh }, area);
}
