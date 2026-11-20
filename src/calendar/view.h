/* Calendar popup views, shared. */
#ifndef CALENDAR_VIEW_H
#define CALENDAR_VIEW_H

#include "nixlytile.h"
#include "netsys.h"
#include "popup_card.h"
#include "calstore/calstore.h"
#include "calendar/calendar.h"

#define GRID_ROWS  6
#define GRID_COLS  7
#define GRID_CELLS (GRID_ROWS * GRID_COLS)
#define PICKS      12
#define ROWS_MAX   24
#define PROVIDERS  8
#define CTRL_GAP   14
#define DATE_GAP   10
#define MARK_W     20
#define ROW_PAD    6
#define WEEKEND    5
#define LINE_LEN   160

typedef enum { FORM_URL, FORM_USER, FORM_SECRET, FORM_SLOTS } FormSlot;

enum {
	HIT_PREV = 200, HIT_NEXT, HIT_TITLE_A, HIT_TITLE_B,
	HIT_WEEK_PREV, HIT_TODAY, HIT_WEEK_NEXT, HIT_DATE,
	HIT_GEAR, HIT_BUBBLE, HIT_TODO, HIT_TARGET,
	HIT_CELL,
	HIT_WEEKNO = HIT_CELL + GRID_CELLS,
	HIT_PICK = HIT_WEEKNO + GRID_ROWS,
	HIT_CHECK = HIT_PICK + PICKS,
	HIT_BACK = HIT_CHECK + ROWS_MAX,
	HIT_REMOVE,
	HIT_PROVIDER = HIT_REMOVE + ACCOUNTS_MAX,
	HIT_FIELD = HIT_PROVIDER + PROVIDERS,
	HIT_CONNECT = HIT_FIELD + FORM_SLOTS,
	HIT_CANCEL,
};

typedef enum { VIEW_MONTH, VIEW_ACCOUNTS } View;
typedef enum { PANE_DAYS, PANE_MONTHS, PANE_YEARS } Pane;
typedef enum { FIELD_NONE, FIELD_DATE, FIELD_TODO, FIELD_FORM } Field;
typedef enum { LOOK_IDLE, LOOK_HOVER, LOOK_ACTIVE, LOOK_BAD } FieldLook;
typedef enum { DATE_IDLE, DATE_TYPING, DATE_BAD } DateState;
typedef enum { ROW_DAY, ROW_ITEM, ROW_EMPTY, ROW_MORE } RowKind;
enum { TONE_PLAIN, TONE_LIVE, TONE_ALERT, TONE_PAST };
enum { DAY_OUTSIDE = 1, DAY_WEEKEND = 2, DAY_TODAY = 4, DAY_SELECTED = 8 };
enum { PICK_CURRENT = 1, PICK_SHOWN = 2 };
enum { BTN_WEEK_PREV, BTN_TODAY, BTN_WEEK_NEXT, BTNS };

typedef CardRect Box;

typedef struct {
	const char *s;
	CardFont font;
	int max_w;
} FitSpec;

typedef struct {
	uint32_t cp;
	uint32_t rgb;
} Glyph;

typedef struct {
	int base, small, big;
	int cell_w, cell_h, wk_w;
	int left_w, right_w, gap, h;
	int nav_h, dow_h, btn_h, grid_y;
	int btn_s, item_h, day_h, more_h;
} Geom;

typedef struct {
	const char *id;
	const char *name;
	const char *icon;
	uint32_t color;
	int oauth;
	const char *fields[FORM_SLOTS];
	const char *hint;
	const char *client_id;
	const char *client_secret;
	const char *link_hint;
} Provider;

typedef struct {
	char num[4];
	uint8_t state;
	uint8_t ndots;
	uint32_t dot[DOTS];
} DayCell;

typedef struct {
	char label[16];
	uint8_t state;
} PickCell;

typedef struct {
	Geom l;
	Pane pane;
	int hover;
	char title_a[32], title_b[24];
	Box prev, next, ta, tb;
	DayCell cell[GRID_CELLS];
	char weekno[GRID_ROWS][4];
	int sel_row;
	PickCell pick[PICKS];
	Box btn[BTNS];
	Box date;
	char date_text[LINE_LEN];
	char date_hint[48];
	DateState date_state;
} MonthModel;

typedef struct {
	uint8_t kind, tone, task, hot;
	uint32_t color;
	int x, y, w, h;
	int mark_y;
	int check;
	char title[128];
	char right[64];
	char sub[LINE_LEN];
} AgendaRow;

typedef struct {
	Geom l;
	int x;
	int hover;
	char status[48];
	uint8_t status_tone, bubble;
	FieldLook todo_look;
	Box gear, bubble_box, todo_box, target_box;
	int list_top;
	char todo[192], target[40];
	int nrows;
	AgendaRow row[ROWS_MAX];
} AgendaModel;

typedef struct {
	char text[64];
	int tone;
} Phrase;

typedef struct {
	int day, seq, task;
	int64_t start;
	const CalItem *it;
	const PendingTodo *pend;
} Entry;

typedef struct {
	const Entry *v;
	int n;
} Upcoming;

typedef struct {
	View view;
	Pane pane;
	int sel;
	int year, month;
	int page;
	int skip;
	int hover;
	Field field;
	int date_error;
	int provider;
	int adding;
	char form[FORM_SLOTS][TEXT_ENTRY_MAX];
	char remove_id[ID_LEN];
	char form_error[192];
} CalUi;

extern CalUi ui;

Geom geom_compute(void);

void fit_text(char *dst, size_t len, FitSpec f);
void fit_tail(char *dst, size_t len, FitSpec f);
char *upcase(char *s);

void paint_rgb(cairo_t *cr, uint32_t rgb, double alpha);
void paint_accent(cairo_t *cr, double alpha);
void paint_wash(cairo_t *cr, Box b, double alpha);
void paint_chevron(cairo_t *cr, Box b, int dir);
void paint_glyph(cairo_t *cr, Box b, Glyph g);
void paint_field(cairo_t *cr, Box b, FieldLook look);
/* Centered; t->x and t->y ignored. */
void paint_text(CardInk *ink, const CardText *t, Box b);

void month_build(MonthModel *mm, const Geom *l);
void month_shapes(cairo_t *cr, CardInk *ink, const MonthModel *mm);
void month_text(CardInk *ink, const MonthModel *mm);
int month_click(int id);
void month_scroll(int steps);
void month_goto(int day);

void agenda_build(AgendaModel *am, const Geom *l);
void agenda_shapes(cairo_t *cr, CardInk *ink, const AgendaModel *am);
void agenda_text(CardInk *ink, const AgendaModel *am);
int agenda_click(int id);
void agenda_scroll(int steps);
void agenda_todo_begin(void);

Upcoming upcoming(int64_t now, int need);

Phrase when_time(const CalItem *it);
Phrase when_until(const CalItem *it, int64_t now);

void accounts_render(Card *card, const Geom *l);
int accounts_click(int id);
void accounts_submit(Field f, const char *text);
/* Saves the field being typed. */
void accounts_commit(void);

const Provider *provider_at(int i);
/* Link form when sign-in is unset. */
const Provider *provider_effective(int i);
const Provider *provider_by_id(const char *id);
const char *provider_name(const char *id);
void provider_tiles(Card *card, int w);

void entry_begin(Field f, const char *label, const char *text);
int entry_on(Field f);
int entry_ours(void);
void calendar_view(View v);

#endif
