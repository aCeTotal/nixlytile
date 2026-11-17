/* Chip text for a cell. */
#include "overview/state.h"

typedef struct {
	const char *text;
	const float *rgba;
} Line;

void
label_describe(Cell *cell)
{
	const char *title = client_get_title(cell->c);

	snprintf(cell->title, sizeof(cell->title), "%s",
			*title ? title : client_get_appid(cell->c));
}

/* Centred; ellipsis only when needed. */
static void
draw_line(struct PixmanBuffer *buf, const Line *line, int row)
{
	struct fcft_font *f = statusfont.font;
	int cap = buf->base.width;
	int w = nixly_text_width(f, line->text);

	nixly_text_draw(buf->image, f, line->text, MAX(cap - w, 0) / 2,
			row * f->height + f->ascent, line->rgba, w > cap ? cap : 0);
}

/* Text strip, capped width. */
void
label_render(Cell *cell, int cap)
{
	struct fcft_font *f = statusfont.font;
	const Line lines[] = {
		{ cell->title, card_col_fg },
		{ cell->detail, cell->kind == DETAIL_REMOTE ? card_col_green : card_col_dim },
	};
	int n = cell->kind == DETAIL_NONE ? 1 : 2;
	struct PixmanBuffer *buf;
	int w = 0;

	if (cap == cell->label_cap)
		return;
	cell->label_cap = cap;
	if (cell->label)
		nx_scene_node_destroy(&cell->label->node);
	cell->label = NULL;
	if (!f || cap <= 0)
		return;

	for (int i = 0; i < n; i++)
		w = MAX(w, nixly_text_width(f, lines[i].text));
	w = MIN(w, cap);
	if (w <= 0 || !(buf = bar_canvas_new(w, n * f->height)))
		return;
	for (int i = 0; i < n; i++)
		draw_line(buf, &lines[i], i);
	cell->label = nx_scene_buffer_create(cell->tree, &buf->base);
	nx_buffer_drop(&buf->base);
	cell->label_w = w;
	cell->label_h = n * f->height;
}
