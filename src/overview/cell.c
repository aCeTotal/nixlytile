/* One window in the grid. */
#include "overview/state.h"

#define SHADOW_SOFT 22
#define SHADOW_DY   8
#define SHADOW_A    0.5f
#define LIFT_GROW   0.035
#define CHIP_INSET  10
#define CHIP_PAD_X  12
#define CHIP_PAD_Y  6
#define CHIP_RADIUS 12

static const float black[4] = { 0, 0, 0, 1 };

static int
label_cap(const Cell *cell)
{
	return cell->slot.width - 2 * (CHIP_INSET + CHIP_PAD_X);
}

/* Whole window, even scrolled off. */
static Snapshot *
capture(Cell *cell)
{
	struct nx_scene_tree *hidden = nx_scene_tree_create(cell->tree);
	struct nx_scene_tree *tree;
	struct nx_box clip;
	Snapshot *s;

	nx_scene_node_set_enabled(&hidden->node, 0);
	tree = nx_scene_subsurface_tree_create(hidden, client_surface(cell->c));
	client_get_clip(cell->c, &clip);
	nx_scene_subsurface_tree_set_clip(&tree->node, &clip);
	s = snapshot_take(cell->tree, tree, clip);
	nx_scene_node_destroy(&hidden->node);
	return s;
}

void
cell_build(Cell *cell)
{
	struct nx_surface *s = client_surface(cell->c);

	cell->tree = nx_scene_tree_create(ov.tiles);
	cell->shadow = shaped_rect(cell->tree,
			&(struct nx_scene_rect_shape){ OV_RADIUS, SHADOW_SOFT, 0 });
	if (s && s->buffer)
		cell->thumb = capture(cell);
	if (!cell->thumb)
		cell->ghost = shaped_rect(cell->tree,
				&(struct nx_scene_rect_shape){ OV_RADIUS, 0, 0 });
	cell->chip = shaped_rect(cell->tree,
			&(struct nx_scene_rect_shape){ CHIP_RADIUS, 0, 0 });
	label_describe(cell);
	label_render(cell, label_cap(cell));
}

void
cell_destroy(Cell *cell)
{
	snapshot_destroy(cell->thumb);
	nx_scene_node_destroy(&cell->tree->node);
}

/* Label follows slot width. */
void
cell_refit(Cell *cell)
{
	label_render(cell, label_cap(cell));
}

/* Label text changed. */
void
cell_relabel(Cell *cell)
{
	cell->label_cap = 0;
	cell_refit(cell);
}

/* Zoom, reflow, then lift. */
static struct nx_box
lifted_box(const Cell *cell)
{
	struct nx_box grid = box_lerp(cell->from, cell->slot, ov.flow.pos);
	struct nx_box b = box_lerp(cell->live, grid, ov.zoom.pos);
	double grow = LIFT_GROW * cell->lift.pos * ov.zoom.pos;
	int dw = (int)lround(b.width * grow), dh = (int)lround(b.height * grow);

	return (struct nx_box){ b.x - dw / 2, b.y - dh / 2,
			MAX(b.width + dw, 1), MAX(b.height + dh, 1) };
}

struct nx_box
cell_box(const Cell *cell)
{
	if (ov.drag.active && cell == &ov.cells[ov.drag.cell])
		return drag_box();
	return lifted_box(cell);
}

static void
place_label(Cell *cell, struct nx_box b, float alpha)
{
	int w = cell->label_w + 2 * CHIP_PAD_X;
	int h = cell->label_h + 2 * CHIP_PAD_Y;
	struct nx_box chip = { b.x + (b.width - w) / 2,
			b.y + b.height - h - CHIP_INSET, w, h };
	int fits = cell->label && w + 2 * CHIP_INSET <= b.width
			&& h + 2 * CHIP_INSET <= b.height;

	nx_scene_node_set_enabled(&cell->chip->node, fits);
	if (cell->label)
		nx_scene_node_set_enabled(&cell->label->node, fits);
	if (!fits)
		return;
	place_rect(cell->chip, chip, (Paint){ statusbar_island_bg, alpha });
	nx_scene_buffer_set_opacity(cell->label, alpha);
	nx_scene_node_set_position(&cell->label->node, chip.x + CHIP_PAD_X,
			chip.y + CHIP_PAD_Y);
}

void
cell_apply(Cell *cell)
{
	struct nx_box b = cell_box(cell);
	float z = (float)ov.zoom.pos;

	place_rect(cell->shadow, (struct nx_box){ b.x - SHADOW_SOFT,
			b.y - SHADOW_SOFT + SHADOW_DY, b.width + 2 * SHADOW_SOFT,
			b.height + 2 * SHADOW_SOFT }, (Paint){ black, SHADOW_A * z });
	if (cell->ghost)
		place_rect(cell->ghost, b, (Paint){ card_col_bg, 1.0f });
	if (cell->thumb) {
		snapshot_round(cell->thumb, (int)lround(OV_RADIUS * z));
		snapshot_fit(cell->thumb, b, b);
	}
	place_label(cell, b, z * z);
}
