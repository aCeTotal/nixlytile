/* Lane for new workspaces. */
#include "overview/state.h"

#define EDGE_W 2
#define FILL_A 0.18f
#define EDGE_A 0.45f
#define HINT   "New workspace"

static void
build(void)
{
	Ghost *g = &ov.ghost;

	g->fill = shaped_rect(ov.chrome,
			&(struct nx_scene_rect_shape){ OV_LANE_R, 0, 0 });
	g->edge = shaped_rect(ov.chrome,
			&(struct nx_scene_rect_shape){ OV_LANE_R, 0, EDGE_W });
	g->badge = shaped_rect(ov.chrome,
			&(struct nx_scene_rect_shape){ OV_BADGE / 2, 0, 0 });
	caption_set(&g->plus, "+", card_col_fg);
	caption_set(&g->hint, HINT, card_col_fg);
}

/* Window already sits alone last. */
static int
pointless(void)
{
	Client *c = ov.cells[ov.drag.cell].c;
	Workspace *last = ov.rows[ov.n_rows - 1].ws;

	return c->column->ws == last && workspace_holds_only(last, c);
}

/* Rows make room below. */
void
ghost_show(void)
{
	if (pointless())
		return;
	if (!ov.ghost.fill)
		build();
	grid_freeze();
	ov.ghost.shown = 1;
	ov.ghost.fading = 0;
	grid_layout(ov.mon);
	ov.ghost.from = ov.ghost.box;
	for (int i = 0; i < ov.n_cells; i++)
		cell_refit(&ov.cells[i]);
	ov.flow = (Motion){ 0.0, 0.0, 1.0 };
}

/* Fades during the next reflow. */
void
ghost_hide(void)
{
	ov.ghost.fading = ov.ghost.shown;
	ov.ghost.shown = 0;
}

static float
ghost_alpha(void)
{
	if (ov.ghost.shown)
		return (float)ov.flow.pos;
	return ov.ghost.fading ? (float)(1.0 - ov.flow.pos) : 0.0f;
}

void
ghost_apply(void)
{
	struct nx_box b = box_lerp(ov.ghost.from, ov.ghost.box, ov.flow.pos);
	struct nx_box badge = lane_badge(b);
	int hot = ov.drag.active && ov.drag.row == ov.n_rows;
	float a = ghost_alpha();

	if (!ov.ghost.fill)
		return;
	place_rect(ov.ghost.fill, b, (Paint){ statusbar_island_bg,
			(hot ? OV_HOT_A : FILL_A) * a });
	place_rect(ov.ghost.edge, b, (Paint){ hot ? focuscolor : card_col_dim,
			(hot ? 1.0f : EDGE_A) * a });
	place_rect(ov.ghost.badge, badge, (Paint){ statusbar_island_bg, a });
	caption_place(&ov.ghost.plus, badge, a);
	caption_place(&ov.ghost.hint, b, a);
}

/* First empty workspace after rows. */
Workspace *
ghost_workspace(void)
{
	struct wl_list *end = &ov.mon->workspaces;
	Workspace *ws = ov.rows[ov.n_rows - 1].ws;

	for (struct wl_list *l = ws->link.next; l != end; l = l->next) {
		ws = wl_container_of(l, ws, link);
		if (!workspace_has_clients(ws))
			return ws;
	}
	return workspace_create(ov.mon);
}
