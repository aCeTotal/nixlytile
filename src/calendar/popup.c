/* Clock popup views and input. */
#include "calendar/view.h"

#include <limits.h>

#define BODY_GAP 8

typedef struct {
	MonthModel m;
	AgendaModel a;
} Body;

CalUi ui = { .provider = -1, .adding = -1 };
CalendarConfig calendar_config;

static Body body;
static int agenda_x;
static Monitor *owner;
static struct wl_event_source *redraw;

static void
body_shapes(cairo_t *cr, CardInk *ink, const void *model)
{
	const Body *b = model;

	month_shapes(cr, ink, &b->m);
	agenda_shapes(cr, ink, &b->a);
}

static void
body_text(CardInk *ink, const void *model)
{
	const Body *b = model;

	month_text(ink, &b->m);
	agenda_text(ink, &b->a);
}

static const CardPainter body_painter = { body_shapes, body_text };

static void
header(Card *card)
{
	time_t t = time(NULL);
	int day = today();
	Civil c = civil_from_day(day);
	char value[16], sub[64];
	struct tm lt;

	localtime_r(&t, &lt);
	strftime(value, sizeof(value), "%H:%M", &lt);
	snprintf(sub, sizeof(sub), "%d %s %d · WEEK %d", c.d, month_name(c.m),
			c.y, iso_week(day));
	card_header(card, clock_icon_path, weekday_name(day), upcase(sub), value);
}

static void
render_month(Card *card, const Geom *l)
{
	header(card);
	card_gap(card, BODY_GAP);
	memset(&body, 0, sizeof(body));
	month_build(&body.m, l);
	agenda_build(&body.a, l);
	agenda_x = CARD_PAD + body.a.x - l->gap / 2;
	card_custom(card, &(CardBlock){ &body_painter, &body, sizeof(body),
			l->left_w + l->gap + l->right_w, l->h });
}

void
calendar_init(void)
{
	sync_init();
}

void
calendar_opening(Monitor *m)
{
	int day = today();
	Civil c = civil_from_day(day);

	if (owner && owner != m && owner->statusbar.clock_popup.visible)
		return;
	owner = m;
	ui.view = job_busy(JOB_ADD) ? VIEW_ACCOUNTS : VIEW_MONTH;
	ui.pane = PANE_DAYS;
	ui.sel = day;
	ui.year = c.y;
	ui.month = c.m;
	ui.skip = 0;
	ui.date_error = 0;
	ui.remove_id[0] = '\0';
	sync_maybe();
}

void
calendar_render(Monitor *m)
{
	InfoPopup *p = &m->statusbar.clock_popup;
	CardResult res;
	Geom l;
	Card *card;

	if (!p->tree || !(card = card_begin()))
		return;
	card_at(m, m->statusbar.area.x + p->tree->node.x,
			m->statusbar.area.y + statusbar_popup_y(m));
	ui.hover = p->btn_hover;
	l = geom_compute();
	if (ui.view == VIEW_ACCOUNTS)
		accounts_render(card, &l);
	else
		render_month(card, &l);
	if (card_finish_sig(card, &res, &p->view.render_sig) != 0)
		return;
	memcpy(p->hits, res.hits, sizeof(p->hits));
	p->nhits = res.nhits;
	popup_view_apply(&p->view, p->tree, &res);
	p->width = p->view.w;
	p->height = p->view.h;
}

static void
redraw_now(void *data)
{
	Monitor *m;

	redraw = NULL;
	wl_list_for_each(m, &mons, link) {
		if (!m->statusbar.clock_popup.visible)
			continue;
		calendar_render(m);
		m->statusbar.clock_popup.last_render_ms = monotonic_msec();
	}
}

/* One redraw per loop turn. */
void
calendar_changed(void)
{
	if (!redraw)
		redraw = wl_event_loop_add_idle(event_loop, redraw_now, NULL);
}

void
calendar_tick(void)
{
	if (ui.view == VIEW_ACCOUNTS)
		calendar_changed();
}

static void
date_submit(const char *text)
{
	int day = parse_date(text, today());

	if (day == INT_MIN) {
		ui.date_error = text[strspn(text, " ")] != '\0';
		return;
	}
	month_goto(day);
}

static void
entry_submit(const char *text, void *data)
{
	Field f = ui.field;

	ui.field = FIELD_NONE;
	if (f == FIELD_DATE)
		date_submit(text);
	if (f == FIELD_TODO)
		todo_add(text, ui.sel);
	if (f >= FIELD_FORM)
		accounts_submit(f, text);
	calendar_changed();
}

int
entry_ours(void)
{
	return text_entry_owner() == entry_submit;
}

int
entry_on(Field f)
{
	return ui.field == f && entry_ours();
}

/* Same field keeps its text. */
void
entry_begin(Field f, const char *label, const char *text)
{
	if (entry_on(f))
		return;
	accounts_commit();
	ui.field = f;
	ui.date_error = 0;
	text_entry_begin(label, f == FIELD_FORM + FORM_SECRET, entry_submit,
			NULL);
	if (text && text[0])
		text_entry_set_text(text);
	calendar_changed();
}

/* Typing never outlives its field. */
void
calendar_view(View v)
{
	if (calendar_holds_open())
		text_entry_cancel();
	ui.view = v;
}

int
calendar_holds_open(void)
{
	return ui.field != FIELD_NONE && entry_ours();
}

void
calendar_entry_changed(void)
{
	if (ui.field == FIELD_NONE)
		return;
	if (!entry_ours())
		ui.field = FIELD_NONE;
	calendar_changed();
}

void
calendar_click(int id)
{
	int handled;

	ui.date_error = 0;
	if (ui.view == VIEW_ACCOUNTS)
		handled = accounts_click(id);
	else
		handled = month_click(id) || agenda_click(id);
	if (!handled && calendar_holds_open())
		text_entry_cancel();
	calendar_changed();
}

void
calendar_scroll(int x, int steps)
{
	if (ui.view != VIEW_MONTH)
		return;
	if (x >= agenda_x)
		agenda_scroll(steps);
	else
		month_scroll(steps);
	calendar_changed();
}

void
calendar_closed(Monitor *m)
{
	int typing = calendar_holds_open();

	if (m != owner)
		return;
	owner = NULL;
	ui.field = FIELD_NONE;
	if (typing)
		text_entry_cancel();
	explicit_bzero(ui.form[FORM_SECRET], sizeof(ui.form[FORM_SECRET]));
}
