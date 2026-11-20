/* Popup geometry from card fonts. */
#include "calendar/view.h"

#define CELL_PAD  24
#define WK_PAD    14
#define COL_GAP   36
#define AGENDA_EM 19
#define NAV_PAD   18
#define DOW_PAD   12
#define BTN_PAD   14
#define ICON_BTN  12
#define ITEM_PAD  12
#define DAY_PAD   14
#define MORE_PAD  10

Geom
geom_compute(void)
{
	Geom l;

	l.base = card_font_height(CARD_FONT_BASE);
	l.small = card_font_height(CARD_FONT_SMALL);
	l.big = card_font_height(CARD_FONT_BIG);
	l.cell_w = MAX(card_font_width(CARD_FONT_BASE, "28") + CELL_PAD,
			l.base * 2);
	l.cell_h = l.base + CELL_PAD;
	l.wk_w = card_font_width(CARD_FONT_CAPS, "WK") + WK_PAD;
	l.left_w = l.wk_w + GRID_COLS * l.cell_w;
	l.gap = COL_GAP;
	l.right_w = MAX(l.base * AGENDA_EM, l.left_w);
	l.nav_h = l.big + NAV_PAD;
	l.dow_h = l.small + DOW_PAD;
	l.btn_h = l.base + BTN_PAD;
	l.grid_y = l.nav_h + l.dow_h;
	l.btn_s = l.base + ICON_BTN;
	l.item_h = l.base + l.small + ITEM_PAD;
	l.day_h = l.small + DAY_PAD;
	l.more_h = l.small + MORE_PAD;
	l.h = l.grid_y + GRID_ROWS * l.cell_h + CTRL_GAP + l.btn_h + DATE_GAP +
		l.btn_h;
	return l;
}
