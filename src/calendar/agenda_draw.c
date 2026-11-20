/* Painting the agenda pane. */
#include "calendar/view.h"

#define RING_R     6.5
#define RING_W     1.6
#define BAR_W      3
#define MARK_INSET  6
#define ARROW      7
#define BUBBLE_R   12.0
#define ICON_PAD   12
#define PLUS_X     15
#define PLUS_ARM   5
#define TODO_X     30

static const float *const tone_cols[] = {
	[TONE_PLAIN] = card_col_dim,
	[TONE_LIVE] = card_col_green,
	[TONE_ALERT] = card_col_red,
	[TONE_PAST] = card_col_faint,
};

static void
bubble_path(cairo_t *cr, Box b, double tip)
{
	double r = BUBBLE_R, x1 = b.x + b.w, y1 = b.y + b.h;

	cairo_new_sub_path(cr);
	cairo_move_to(cr, b.x + r, b.y);
	cairo_line_to(cr, tip - ARROW, b.y);
	cairo_line_to(cr, tip, b.y - ARROW);
	cairo_line_to(cr, tip + ARROW, b.y);
	cairo_arc(cr, x1 - r, b.y + r, r, -M_PI / 2, 0);
	cairo_arc(cr, x1 - r, y1 - r, r, 0, M_PI / 2);
	cairo_arc(cr, b.x + r, y1 - r, r, M_PI / 2, M_PI);
	cairo_arc(cr, b.x + r, b.y + r, r, M_PI, 3 * M_PI / 2);
	cairo_close_path(cr);
}

static Box
bubble_icon(const AgendaModel *am)
{
	Box b = am->bubble_box;
	int isz = am->l.base + 6;

	return (Box){ b.x + ICON_PAD, b.y + (b.h - isz) / 2, isz, isz };
}

static void
shapes_bubble(cairo_t *cr, CardInk *ink, const AgendaModel *am)
{
	bubble_path(cr, am->bubble_box, am->gear.x + am->gear.w / 2.0);
	paint_accent(cr, am->hover == HIT_BUBBLE ? 0.26 : 0.17);
	cairo_fill_preserve(cr);
	paint_accent(cr, 0.55);
	cairo_set_line_width(cr, 1.0);
	cairo_stroke(cr);
	card_draw_icon(cr, "cal-sync", bubble_icon(am));
	card_ink_hit(ink, am->bubble_box, HIT_BUBBLE);
}

static void
plus(cairo_t *cr, double cx, double cy)
{
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_set_line_width(cr, 1.8);
	cairo_move_to(cr, cx - PLUS_ARM, cy);
	cairo_line_to(cr, cx + PLUS_ARM, cy);
	cairo_move_to(cr, cx, cy - PLUS_ARM);
	cairo_line_to(cr, cx, cy + PLUS_ARM);
	paint_accent(cr, 0.95);
	cairo_stroke(cr);
}

static void
shapes_todo(cairo_t *cr, CardInk *ink, const AgendaModel *am)
{
	Box t = am->todo_box;

	paint_field(cr, t, am->todo_look);
	plus(cr, t.x + PLUS_X, t.y + t.h / 2.0);
	paint_wash(cr, am->target_box, am->hover == HIT_TARGET ? 0.16 : 0.08);
	card_ink_hit(ink, am->target_box, HIT_TARGET);
	card_ink_hit(ink, t, HIT_TODO);
}

static void
ring_path(cairo_t *cr, const AgendaRow *r)
{
	cairo_new_sub_path(cr);
	cairo_arc(cr, r->x + RING_R + 1, r->mark_y, RING_R, 0, 2 * M_PI);
}

static void
ring_outline(cairo_t *cr, const AgendaRow *r)
{
	ring_path(cr, r);
	paint_rgb(cr, r->color, 0.9);
	cairo_set_line_width(cr, RING_W);
	cairo_stroke(cr);
}

static void
ring_checked(cairo_t *cr, const AgendaRow *r)
{
	double cx = r->x + RING_R + 1, cy = r->mark_y;

	ring_path(cr, r);
	paint_rgb(cr, r->color, 1);
	cairo_fill(cr);
	cairo_move_to(cr, cx - 3, cy);
	cairo_line_to(cr, cx - 0.8, cy + 2.4);
	cairo_line_to(cr, cx + 3.2, cy - 2.6);
	cairo_set_source_rgba(cr, card_col_bg[0], card_col_bg[1],
			card_col_bg[2], 1);
	cairo_set_line_width(cr, RING_W);
	cairo_stroke(cr);
}

static void
day_rule(cairo_t *cr, const AgendaRow *r)
{
	int tw = card_font_width(CARD_FONT_CAPS, r->title) + 10;

	cairo_rectangle(cr, r->x + tw, r->y + r->h / 2 + 2, r->w - tw, 1);
	cairo_set_source_rgba(cr, 1, 1, 1, 0.07);
	cairo_fill(cr);
}

static void
shapes_item(cairo_t *cr, CardInk *ink, const AgendaRow *r)
{
	if (r->tone == TONE_LIVE && !r->task)
		paint_wash(cr, (Box){ r->x - ROW_PAD, r->y + 1,
				r->w + 2 * ROW_PAD, r->h - 2 }, 0.05);
	if (r->task && r->hot)
		ring_checked(cr, r);
	else if (r->task)
		ring_outline(cr, r);
	else {
		card_rounded(cr, (CardRound){ r->x + 2, r->y + MARK_INSET, BAR_W,
				r->h - 2 * MARK_INSET, BAR_W / 2.0 });
		paint_rgb(cr, r->color, 1);
		cairo_fill(cr);
	}
	if (r->check >= 0)
		card_ink_hit(ink, (Box){ r->x - 4, r->y, MARK_W + 4, r->h },
				r->check);
}

void
agenda_shapes(cairo_t *cr, CardInk *ink, const AgendaModel *am)
{
	cairo_rectangle(cr, am->x - am->l.gap / 2, ROW_PAD, 1,
			am->l.h - 2 * ROW_PAD);
	cairo_set_source_rgba(cr, 1, 1, 1, 0.07);
	cairo_fill(cr);
	if (am->hover == HIT_GEAR)
		paint_wash(cr, am->gear, 0.10);
	card_draw_icon(cr, "cal-gear", (Box){ am->gear.x + 5, am->gear.y + 5,
			am->gear.w - 10, am->gear.h - 10 });
	card_ink_hit(ink, am->gear, HIT_GEAR);
	if (am->bubble)
		shapes_bubble(cr, ink, am);
	shapes_todo(cr, ink, am);
	for (int i = 0; i < am->nrows; i++) {
		const AgendaRow *r = &am->row[i];

		if (r->kind == ROW_DAY)
			day_rule(cr, r);
		if (r->kind == ROW_ITEM)
			shapes_item(cr, ink, r);
	}
}

static void
text_note(CardInk *ink, const AgendaRow *r, const CardText *t)
{
	paint_text(ink, t, (Box){ r->x, r->y, r->w, r->h });
}

static void
text_item(CardInk *ink, const AgendaModel *am, const AgendaRow *r)
{
	const Geom *l = &am->l;
	int y = r->y + ROW_PAD;
	int rw = card_font_width(CARD_FONT_SMALL, r->right);

	card_ink_text(ink, &(CardText){ CARD_FONT_BASE, r->title, r->x + MARK_W,
			y, r->tone == TONE_PAST ? card_col_dim : card_col_fg });
	card_ink_text(ink, &(CardText){ CARD_FONT_SMALL, r->right,
			r->x + r->w - rw, y + (l->base - l->small) / 2, card_col_dim });
	card_ink_text(ink, &(CardText){ CARD_FONT_SMALL, r->sub, r->x + MARK_W,
			y + l->base + 2, tone_cols[r->tone] });
}

static void
text_row(CardInk *ink, const AgendaModel *am, const AgendaRow *r)
{
	switch (r->kind) {
	case ROW_DAY:
		card_ink_text(ink, &(CardText){ CARD_FONT_CAPS, r->title, r->x,
				r->y + (r->h - am->l.small) / 2 + 2,
				r->tone == TONE_LIVE ? card_col_blue : card_col_faint });
		break;
	case ROW_MORE:
		text_note(ink, r, &(CardText){ CARD_FONT_SMALL, "Scroll for more",
				0, 0, card_col_faint });
		break;
	case ROW_EMPTY:
		text_note(ink, r, &(CardText){ CARD_FONT_BASE, "Nothing planned",
				0, 0, card_col_faint });
		break;
	case ROW_ITEM:
		text_item(ink, am, r);
		break;
	}
}

static void
text_bubble(CardInk *ink, const AgendaModel *am)
{
	const Geom *l = &am->l;
	Box b = am->bubble_box, icon = bubble_icon(am);
	int tx = icon.x + icon.w + ICON_PAD;
	int ty = b.y + (b.h - l->base - l->small - 4) / 2;
	char sub[128];

	fit_text(sub, sizeof(sub), (FitSpec){
		"Click here · Outlook, Google, iCloud and more",
		CARD_FONT_SMALL, b.x + b.w - tx - ICON_PAD });
	card_ink_text(ink, &(CardText){ CARD_FONT_BASE,
			"Sync your other calendars", tx, ty, card_col_fg });
	card_ink_text(ink, &(CardText){ CARD_FONT_SMALL, sub, tx,
			ty + l->base + 4, card_col_dim });
}

static void
text_controls(CardInk *ink, const AgendaModel *am)
{
	const Geom *l = &am->l;
	Box t = am->todo_box;
	int sw = card_font_width(CARD_FONT_SMALL, am->status);
	int sy = (l->nav_h - l->small) / 2;

	card_ink_text(ink, &(CardText){ CARD_FONT_CAPS, "AGENDA", am->x, sy,
			card_col_faint });
	card_ink_text(ink, &(CardText){ CARD_FONT_SMALL, am->status,
			am->gear.x - 8 - sw, sy, tone_cols[am->status_tone] });
	if (am->bubble)
		text_bubble(ink, am);
	card_ink_text(ink, &(CardText){ CARD_FONT_BASE, am->todo, t.x + TODO_X,
			t.y + (t.h - l->base) / 2,
			am->todo_look == LOOK_ACTIVE ? card_col_fg : card_col_faint });
	paint_text(ink, &(CardText){ CARD_FONT_SMALL, am->target, 0, 0,
			card_col_dim }, am->target_box);
}

void
agenda_text(CardInk *ink, const AgendaModel *am)
{
	text_controls(ink, am);
	for (int i = 0; i < am->nrows; i++)
		text_row(ink, am, &am->row[i]);
}
