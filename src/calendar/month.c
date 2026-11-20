/* Month pane: model and navigation. */
#include "calendar/view.h"

#define BTN_GAP    8
#define YEAR_LEAD  4
#define TITLE_PAD  16
#define FIELD_PAD  24

static const char *const date_texts[] = {
	[DATE_IDLE] = "Go to date…",
	[DATE_BAD] = "Unknown date — try 24.12 or +7",
};

static int
grid_first(void)
{
	int first = day_from_civil((Civil){ ui.year, ui.month, 1 });

	return first - weekday(first);
}

static void
build_nav(MonthModel *mm, const Geom *l)
{
	int s = l->btn_s;
	int wa = card_font_width(CARD_FONT_BIG, mm->title_a) + TITLE_PAD;
	int wb = mm->title_b[0] ?
		card_font_width(CARD_FONT_BIG, mm->title_b) + TITLE_PAD : 0;
	int x = (l->left_w - wa - wb) / 2;

	mm->prev = (Box){ 0, (l->nav_h - s) / 2, s, s };
	mm->next = (Box){ l->left_w - s, mm->prev.y, s, s };
	mm->ta = (Box){ x, (l->nav_h - l->big - 8) / 2, wa, l->big + 8 };
	mm->tb = (Box){ x + wa, mm->ta.y, wb, mm->ta.h };
}

static void
build_titles(MonthModel *mm)
{
	if (ui.pane == PANE_DAYS) {
		snprintf(mm->title_a, sizeof(mm->title_a), "%s",
				month_name(ui.month));
		snprintf(mm->title_b, sizeof(mm->title_b), "%d", ui.year);
		return;
	}
	if (ui.pane == PANE_MONTHS) {
		snprintf(mm->title_a, sizeof(mm->title_a), "%d", ui.year);
		return;
	}
	snprintf(mm->title_a, sizeof(mm->title_a), "%d – %d", ui.page,
			ui.page + PICKS - 1);
}

static void
build_days(MonthModel *mm)
{
	int first = grid_first(), now = today();
	DayMarks marks[GRID_CELLS];

	store_marks(first, marks, GRID_CELLS);
	for (int i = 0; i < GRID_CELLS; i++) {
		int day = first + i;
		Civil c = civil_from_day(day);
		DayCell *cell = &mm->cell[i];

		snprintf(cell->num, sizeof(cell->num), "%d", c.d);
		cell->state = (c.m != ui.month ? DAY_OUTSIDE : 0) |
			(weekday(day) >= WEEKEND ? DAY_WEEKEND : 0) |
			(day == now ? DAY_TODAY : 0) |
			(day == ui.sel ? DAY_SELECTED : 0);
		cell->ndots = (uint8_t)marks[i].n;
		memcpy(cell->dot, marks[i].color, sizeof(cell->dot));
	}
	for (int r = 0; r < GRID_ROWS; r++)
		snprintf(mm->weekno[r], sizeof(mm->weekno[r]), "%d",
				iso_week(first + r * GRID_COLS));
	mm->sel_row = ui.sel >= first && ui.sel < first + GRID_CELLS ?
		(ui.sel - first) / GRID_COLS : -1;
}

static uint8_t
month_state(int i, Civil now)
{
	return (ui.year == now.y && i + 1 == now.m ? PICK_CURRENT : 0) |
		(i + 1 == ui.month ? PICK_SHOWN : 0);
}

static void
build_picks(MonthModel *mm)
{
	Civil now = civil_from_day(today());

	for (int i = 0; i < PICKS; i++) {
		PickCell *p = &mm->pick[i];
		int year = ui.page + i;

		if (ui.pane == PANE_MONTHS) {
			snprintf(p->label, sizeof(p->label), "%.3s",
					month_name(i + 1));
			p->state = month_state(i, now);
			continue;
		}
		snprintf(p->label, sizeof(p->label), "%d", year);
		p->state = (year == now.y ? PICK_CURRENT : 0) |
			(year == ui.year ? PICK_SHOWN : 0);
	}
}

static void
build_controls(MonthModel *mm, const Geom *l)
{
	int y = l->grid_y + GRID_ROWS * l->cell_h + CTRL_GAP;
	int w = (l->left_w - (BTNS - 1) * BTN_GAP) / BTNS;

	for (int i = 0; i < BTNS; i++)
		mm->btn[i] = (Box){ i * (w + BTN_GAP), y, w, l->btn_h };
	mm->date = (Box){ 0, y + l->btn_h + DATE_GAP, l->left_w, l->btn_h };
	mm->date_state = entry_on(FIELD_DATE) ? DATE_TYPING :
		ui.date_error ? DATE_BAD : DATE_IDLE;
	if (mm->date_state == DATE_IDLE)
		snprintf(mm->date_hint, sizeof(mm->date_hint),
				"24.12 · +7 · tomorrow");
	fit_tail(mm->date_text, sizeof(mm->date_text), (FitSpec){
		mm->date_state == DATE_TYPING ? text_entry_display() :
		date_texts[mm->date_state], CARD_FONT_BASE,
		l->left_w - FIELD_PAD });
}

void
month_build(MonthModel *mm, const Geom *l)
{
	mm->l = *l;
	mm->pane = ui.pane;
	mm->hover = ui.hover;
	build_titles(mm);
	build_nav(mm, l);
	if (ui.pane == PANE_DAYS)
		build_days(mm);
	else
		build_picks(mm);
	build_controls(mm, l);
}

static int
clamp_year(int y)
{
	return MAX(YEAR_MIN, MIN(y, YEAR_MAX));
}

/* Start of a full page. */
static int
clamp_page(int y)
{
	return MAX(YEAR_MIN, MIN(y, YEAR_MAX - PICKS + 1));
}

static void
show_day(int day)
{
	Civil c = civil_from_day(day);

	if (c.y != clamp_year(c.y))
		return;
	ui.sel = day;
	ui.year = c.y;
	ui.month = c.m;
	ui.skip = 0;
}

void
month_goto(int day)
{
	show_day(day);
	ui.pane = PANE_DAYS;
}

static void
step_month(int n)
{
	int idx = ui.year * MONTHS + ui.month - 1 + n;
	int y = idx / MONTHS;

	if (y != clamp_year(y))
		return;
	ui.year = y;
	ui.month = idx - y * MONTHS + 1;
}

static void
nav_step(int dir)
{
	if (ui.pane == PANE_DAYS)
		step_month(dir);
	else if (ui.pane == PANE_MONTHS)
		ui.year = clamp_year(ui.year + dir);
	else
		ui.page = clamp_page(ui.page + dir * PICKS);
}

static void
title_click(int id)
{
	if (ui.pane == PANE_DAYS && id == HIT_TITLE_A) {
		ui.pane = PANE_MONTHS;
		return;
	}
	if (ui.pane == PANE_YEARS) {
		ui.pane = PANE_DAYS;
		return;
	}
	ui.page = clamp_page(ui.year - YEAR_LEAD);
	ui.pane = PANE_YEARS;
}

static void
pick_click(int i)
{
	if (ui.pane == PANE_MONTHS) {
		ui.month = i + 1;
		ui.pane = PANE_DAYS;
		return;
	}
	ui.year = clamp_year(ui.page + i);
	ui.pane = PANE_MONTHS;
}

/* Second click adds a to-do. */
static void
cell_click(int i)
{
	int day = grid_first() + i;

	if (day == ui.sel) {
		agenda_todo_begin();
		return;
	}
	show_day(day);
}

static int
range_click(int id)
{
	if (id >= HIT_CELL && id < HIT_CELL + GRID_CELLS) {
		cell_click(id - HIT_CELL);
		return 1;
	}
	if (id >= HIT_WEEKNO && id < HIT_WEEKNO + GRID_ROWS) {
		show_day(grid_first() + (id - HIT_WEEKNO) * GRID_COLS);
		return 1;
	}
	if (id < HIT_PICK || id >= HIT_PICK + PICKS)
		return 0;
	pick_click(id - HIT_PICK);
	return 1;
}

int
month_click(int id)
{
	switch (id) {
	case HIT_PREV:
	case HIT_NEXT:
		nav_step(id == HIT_PREV ? -1 : 1);
		return 1;
	case HIT_TITLE_A:
	case HIT_TITLE_B:
		title_click(id);
		return 1;
	case HIT_WEEK_PREV:
	case HIT_WEEK_NEXT:
		show_day(ui.sel + (id == HIT_WEEK_PREV ? -GRID_COLS : GRID_COLS));
		return 1;
	case HIT_TODAY:
		month_goto(today());
		return 1;
	case HIT_DATE:
		entry_begin(FIELD_DATE, "Date", "");
		return 1;
	default:
		return range_click(id);
	}
}

void
month_scroll(int steps)
{
	nav_step(-steps);
}
