/* Painting the month pane. */
#include "calendar/view.h"

#define PICK_COLS   3
#define PICK_ROWS   4
#define DISC_PAD    10
#define DOT_R       2.2
#define DOT_STEP    6.0
#define PICK_R      10.0
#define BTN_R       8.0
#define FIELD_INSET 12

static const char *const dow[GRID_COLS] = {
	"MO", "TU", "WE", "TH", "FR", "SA", "SU",
};
static const char *const week_buttons[BTNS] = {
	[BTN_WEEK_PREV] = "‹  Week", [BTN_TODAY] = "Today",
	[BTN_WEEK_NEXT] = "Week  ›",
};

static Box
cell_box(const Geom *l, int i)
{
	return (Box){ l->wk_w + i % GRID_COLS * l->cell_w,
		l->grid_y + i / GRID_COLS * l->cell_h, l->cell_w, l->cell_h };
}

static Box
disc_box(const Geom *l, Box c)
{
	int d = l->base + DISC_PAD;

	return (Box){ c.x + (c.w - d) / 2, c.y + 4, d, d };
}

static Box
pick_box(const Geom *l, int i)
{
	int w = l->left_w / PICK_COLS, h = GRID_ROWS * l->cell_h / PICK_ROWS;

	return (Box){ i % PICK_COLS * w + 4, l->grid_y + i / PICK_COLS * h + 4,
		w - 8, h - 8 };
}

static Box
weekno_box(const Geom *l, int r)
{
	return (Box){ 0, l->grid_y + r * l->cell_h, l->wk_w, l->cell_h };
}

static void
disc(cairo_t *cr, Box b, double grow)
{
	cairo_new_sub_path(cr);
	cairo_arc(cr, b.x + b.w / 2.0, b.y + b.h / 2.0, b.w / 2.0 + grow, 0,
			2 * M_PI);
}

static void
shapes_cell(cairo_t *cr, const MonthModel *mm, int i)
{
	const DayCell *cell = &mm->cell[i];
	Box d = disc_box(&mm->l, cell_box(&mm->l, i));
	double x0 = d.x + d.w / 2.0 - (cell->ndots - 1) * DOT_STEP / 2;

	if (cell->state & DAY_TODAY) {
		disc(cr, d, 0);
		paint_accent(cr, 0.95);
		cairo_fill(cr);
	} else if (mm->hover == HIT_CELL + i) {
		disc(cr, d, 0);
		cairo_set_source_rgba(cr, 1, 1, 1, 0.10);
		cairo_fill(cr);
	}
	if (cell->state & DAY_SELECTED) {
		disc(cr, d, cell->state & DAY_TODAY ? 3 : 0);
		paint_accent(cr, 0.9);
		cairo_set_line_width(cr, 1.6);
		cairo_stroke(cr);
	}
	for (int k = 0; k < cell->ndots; k++) {
		cairo_new_sub_path(cr);
		cairo_arc(cr, x0 + k * DOT_STEP, d.y + d.h + 5, DOT_R, 0,
				2 * M_PI);
		paint_rgb(cr, cell->dot[k], cell->state & DAY_OUTSIDE ? 0.45 : 1);
		cairo_fill(cr);
	}
}

static void
shapes_days(cairo_t *cr, CardInk *ink, const MonthModel *mm)
{
	const Geom *l = &mm->l;

	if (mm->sel_row >= 0)
		paint_wash(cr, (Box){ 0, l->grid_y + mm->sel_row * l->cell_h + 1,
				l->left_w, l->cell_h - 2 }, 0.045);
	for (int r = 0; r < GRID_ROWS; r++) {
		if (mm->hover == HIT_WEEKNO + r)
			paint_wash(cr, weekno_box(l, r), 0.08);
		card_ink_hit(ink, weekno_box(l, r), HIT_WEEKNO + r);
	}
	for (int i = 0; i < GRID_CELLS; i++) {
		shapes_cell(cr, mm, i);
		card_ink_hit(ink, cell_box(l, i), HIT_CELL + i);
	}
}

static void
shapes_picks(cairo_t *cr, CardInk *ink, const MonthModel *mm)
{
	for (int i = 0; i < PICKS; i++) {
		Box b = pick_box(&mm->l, i);

		card_rounded(cr, (CardRound){ b.x, b.y, b.w, b.h, PICK_R });
		if (mm->pick[i].state & PICK_SHOWN)
			paint_accent(cr, 0.22);
		else
			cairo_set_source_rgba(cr, 1, 1, 1,
					mm->hover == HIT_PICK + i ? 0.10 : 0.03);
		cairo_fill_preserve(cr);
		paint_accent(cr, mm->pick[i].state & PICK_CURRENT ? 0.9 : 0);
		cairo_set_line_width(cr, 1.4);
		cairo_stroke(cr);
		card_ink_hit(ink, b, HIT_PICK + i);
	}
}

static void
shapes_controls(cairo_t *cr, CardInk *ink, const MonthModel *mm)
{
	static const FieldLook looks[] = {
		[DATE_IDLE] = LOOK_IDLE, [DATE_TYPING] = LOOK_ACTIVE,
		[DATE_BAD] = LOOK_BAD,
	};
	FieldLook look = looks[mm->date_state];

	for (int i = 0; i < BTNS; i++) {
		Box b = mm->btn[i];
		int hot = mm->hover == HIT_WEEK_PREV + i;

		if (i == BTN_TODAY) {
			card_rounded(cr, (CardRound){ b.x, b.y, b.w, b.h, BTN_R });
			paint_accent(cr, hot ? 0.34 : 0.20);
			cairo_fill(cr);
		} else {
			paint_wash(cr, b, hot ? 0.14 : 0.07);
		}
		card_ink_hit(ink, b, HIT_WEEK_PREV + i);
	}
	if (look == LOOK_IDLE && mm->hover == HIT_DATE)
		look = LOOK_HOVER;
	paint_field(cr, mm->date, look);
	card_ink_hit(ink, mm->date, HIT_DATE);
}

void
month_shapes(cairo_t *cr, CardInk *ink, const MonthModel *mm)
{
	for (int i = 0; i < 2; i++) {
		Box b = i ? mm->next : mm->prev;

		if (mm->hover == HIT_PREV + i)
			paint_wash(cr, b, 0.10);
		paint_chevron(cr, b, i ? 1 : -1);
		card_ink_hit(ink, b, HIT_PREV + i);
	}
	if (mm->hover == HIT_TITLE_A)
		paint_wash(cr, mm->ta, 0.08);
	if (mm->hover == HIT_TITLE_B && mm->tb.w)
		paint_wash(cr, mm->tb, 0.08);
	card_ink_hit(ink, mm->ta, HIT_TITLE_A);
	if (mm->tb.w)
		card_ink_hit(ink, mm->tb, HIT_TITLE_B);
	if (mm->pane == PANE_DAYS)
		shapes_days(cr, ink, mm);
	else
		shapes_picks(cr, ink, mm);
	shapes_controls(cr, ink, mm);
}

static const float *
day_color(const DayCell *cell)
{
	if (cell->state & DAY_TODAY)
		return card_col_bg;
	if (cell->state & DAY_SELECTED)
		return card_col_blue;
	if (cell->state & DAY_OUTSIDE)
		return card_col_faint;
	return cell->state & DAY_WEEKEND ? card_col_dim : card_col_fg;
}

static void
text_days(CardInk *ink, const MonthModel *mm)
{
	const Geom *l = &mm->l;

	paint_text(ink, &(CardText){ CARD_FONT_CAPS, "WK", 0, 0,
			card_col_faint }, (Box){ 0, l->nav_h, l->wk_w, l->dow_h });
	for (int c = 0; c < GRID_COLS; c++)
		paint_text(ink, &(CardText){ CARD_FONT_CAPS, dow[c], 0, 0,
				c >= WEEKEND ? card_col_faint : card_col_dim },
				(Box){ l->wk_w + c * l->cell_w, l->nav_h,
				l->cell_w, l->dow_h });
	for (int r = 0; r < GRID_ROWS; r++)
		paint_text(ink, &(CardText){ CARD_FONT_SMALL, mm->weekno[r], 0, 0,
				r == mm->sel_row ? card_col_dim : card_col_faint },
				disc_box(l, weekno_box(l, r)));
	for (int i = 0; i < GRID_CELLS; i++)
		paint_text(ink, &(CardText){ CARD_FONT_BASE, mm->cell[i].num,
				0, 0, day_color(&mm->cell[i]) },
				disc_box(l, cell_box(l, i)));
}

static void
text_picks(CardInk *ink, const MonthModel *mm)
{
	for (int i = 0; i < PICKS; i++)
		paint_text(ink, &(CardText){ CARD_FONT_BASE, mm->pick[i].label,
				0, 0, mm->pick[i].state & PICK_SHOWN ?
				card_col_fg : card_col_dim },
				pick_box(&mm->l, i));
}

static void
text_controls(CardInk *ink, const MonthModel *mm)
{
	static const float *const date_cols[] = {
		[DATE_IDLE] = card_col_faint, [DATE_TYPING] = card_col_fg,
		[DATE_BAD] = card_col_red,
	};
	Box d = mm->date;
	int ty = d.y + (d.h - mm->l.base) / 2;

	for (int i = 0; i < BTNS; i++)
		paint_text(ink, &(CardText){ CARD_FONT_BASE, week_buttons[i], 0, 0,
				card_col_fg }, mm->btn[i]);
	card_ink_text(ink, &(CardText){ CARD_FONT_BASE, mm->date_text,
			d.x + FIELD_INSET, ty, date_cols[mm->date_state] });
	card_ink_text(ink, &(CardText){ CARD_FONT_SMALL, mm->date_hint,
			d.x + d.w - FIELD_INSET -
			card_font_width(CARD_FONT_SMALL, mm->date_hint),
			d.y + (d.h - mm->l.small) / 2, card_col_faint });
}

void
month_text(CardInk *ink, const MonthModel *mm)
{
	paint_text(ink, &(CardText){ CARD_FONT_BIG, mm->title_a, 0, 0,
			card_col_fg }, mm->ta);
	if (mm->tb.w)
		paint_text(ink, &(CardText){ CARD_FONT_BIG, mm->title_b, 0, 0,
				card_col_dim }, mm->tb);
	if (mm->pane == PANE_DAYS)
		text_days(ink, mm);
	else
		text_picks(ink, mm);
	text_controls(ink, mm);
}
