/* Agenda pane: model and clicks. */
#include "calendar/view.h"

#include <limits.h>

#define ROW_GAP       8
#define PENDING_COLOR 0x8b93a6
#define RIGHT_GAP     12
#define CHIP_PAD      22
#define CHIP_INSET    5
#define TODO_INSET    30

static char row_ref[ROWS_MAX][REF_LEN];

static void
day_label(AgendaRow *r, int day, int now_day)
{
	static const char *const near[] = { "YESTERDAY · ", "TODAY · ",
		"TOMORROW · " };
	Civil c = civil_from_day(day);
	int rel = day - now_day + 1;

	snprintf(r->title, sizeof(r->title), "%s%s %d %.3s",
			rel >= 0 && rel < (int)LENGTH(near) ? near[rel] : "",
			weekday_name(day), c.d, month_name(c.m));
	upcase(r->title);
	r->tone = day == now_day ? TONE_LIVE : TONE_PAST;
}

static int
next_y(const AgendaModel *am)
{
	if (!am->nrows)
		return am->list_top;
	return am->row[am->nrows - 1].y + am->row[am->nrows - 1].h;
}

static AgendaRow *
row_add(AgendaModel *am, RowKind kind, int h)
{
	AgendaRow *r = &am->row[am->nrows];

	r->kind = (uint8_t)kind;
	r->x = am->x;
	r->y = next_y(am);
	r->w = am->l.right_w;
	r->h = h;
	r->check = -1;
	am->nrows++;
	return r;
}

static void
fill_pending(AgendaRow *r, const PendingTodo *p)
{
	r->task = 1;
	r->tone = TONE_PAST;
	r->color = PENDING_COLOR;
	fit_text(r->title, sizeof(r->title), (FitSpec){ p->title,
			CARD_FONT_BASE, r->w - MARK_W });
	snprintf(r->sub, sizeof(r->sub), "Saving…");
}

static void
fill_item(AgendaRow *r, const CalItem *it, int64_t now)
{
	const char *where = store_str(it->where)[0] ? store_str(it->where) :
		store_str(it->cal);
	Phrase until = when_until(it, now), at = when_time(it);
	char sub[256];

	snprintf(sub, sizeof(sub), where[0] ? "%s · %s" : "%s", until.text,
			where);
	snprintf(r->right, sizeof(r->right), "%s", at.text);
	fit_text(r->title, sizeof(r->title), (FitSpec){ store_str(it->title),
			CARD_FONT_BASE, r->w - MARK_W - RIGHT_GAP -
			card_font_width(CARD_FONT_SMALL, r->right) });
	fit_text(r->sub, sizeof(r->sub), (FitSpec){ sub, CARD_FONT_SMALL,
			r->w - MARK_W });
	r->tone = (uint8_t)until.tone;
	r->color = it->color;
	r->task = (it->flags & CAL_TASK) != 0;
}

static void
add_entry(AgendaModel *am, const Entry *e, int64_t now)
{
	const Geom *l = &am->l;
	AgendaRow *r = row_add(am, ROW_ITEM, l->item_h);
	int i = am->nrows - 1;

	r->mark_y = r->y + ROW_PAD + l->base / 2;
	snprintf(row_ref[i], REF_LEN, "%s", e->it ? store_str(e->it->ref) : "");
	if (e->pend) {
		fill_pending(r, e->pend);
		return;
	}
	fill_item(r, e->it, now);
	r->check = r->task && row_ref[i][0] ? HIT_CHECK + i : -1;
	r->hot = r->check >= 0 && am->hover == r->check;
}

static void
build_rows(AgendaModel *am, int64_t now)
{
	const Geom *l = &am->l;
	Upcoming up = upcoming(now, ui.skip + ROWS_MAX + 1);
	int bottom = am->todo_box.y - ROW_GAP;
	int now_day = day_of(now), last = INT_MIN;

	ui.skip = MIN(ui.skip, MAX(up.n - 1, 0));
	for (int i = ui.skip; i < up.n && am->nrows < ROWS_MAX - 2; i++) {
		const Entry *e = &up.v[i];
		int head = e->day != last ? l->day_h : 0;
		int tail = i + 1 < up.n ? l->more_h : 0;

		if (next_y(am) + head + l->item_h > bottom - tail) {
			row_add(am, ROW_MORE, l->more_h);
			return;
		}
		if (head)
			day_label(row_add(am, ROW_DAY, l->day_h), e->day, now_day);
		last = e->day;
		add_entry(am, e, now);
	}
	if (!up.n)
		row_add(am, ROW_EMPTY, l->item_h);
}

static int
account_failed(void)
{
	for (int i = 0; i < store_account_count(); i++) {
		const AccountState *s = account_state(store_account(i)->id);

		if (s && s->error[0])
			return 1;
	}
	return sync_last_error()[0] != '\0';
}

static void
build_status(AgendaModel *am, int64_t now)
{
	int64_t age = now - sync_last_ok();

	am->status_tone = TONE_PAST;
	if (store_account_count() && account_failed()) {
		am->status_tone = TONE_ALERT;
		snprintf(am->status, sizeof(am->status), "Sync issue");
		return;
	}
	if (!sync_last_ok()) {
		snprintf(am->status, sizeof(am->status), "%s",
				sync_running() ? "Syncing…" : "");
		return;
	}
	if (age < MIN_S) {
		snprintf(am->status, sizeof(am->status), "Synced just now");
		return;
	}
	snprintf(am->status, sizeof(am->status), "Synced %lld %s ago",
			(long long)(age < HOUR_S ? age / MIN_S : age / HOUR_S),
			age < HOUR_S ? "min" : "h");
}

static const char *
target_name(void)
{
	const CalAccount *a = store_account_by_id(todo_target());

	return a ? provider_name(a->provider) : "Local";
}

static void
build_todo(AgendaModel *am, int y)
{
	const Geom *l = &am->l;
	Civil c = civil_from_day(ui.sel);
	int cw;

	am->todo_box = (Box){ am->x, y, l->right_w, l->btn_h };
	snprintf(am->target, sizeof(am->target), "→ %s", target_name());
	cw = card_font_width(CARD_FONT_SMALL, am->target) + CHIP_PAD;
	am->target_box = (Box){ am->x + l->right_w - cw - CHIP_INSET,
		y + CHIP_INSET, cw, l->btn_h - 2 * CHIP_INSET };
	am->todo_look = entry_on(FIELD_TODO) ? LOOK_ACTIVE :
		am->hover == HIT_TODO ? LOOK_HOVER : LOOK_IDLE;
	if (am->todo_look == LOOK_ACTIVE)
		fit_tail(am->todo, sizeof(am->todo), (FitSpec){
			text_entry_display(), CARD_FONT_BASE,
			l->right_w - TODO_INSET - cw - 2 * CHIP_INSET });
	else
		snprintf(am->todo, sizeof(am->todo), "Add to-do · %.3s %d %.3s",
				weekday_name(ui.sel), c.d, month_name(c.m));
}

static void
build_controls(AgendaModel *am)
{
	const Geom *l = &am->l;
	int s = l->btn_s, y = l->nav_h;

	am->gear = (Box){ am->x + l->right_w - s, (l->nav_h - s) / 2, s, s };
	am->bubble = store_account_count() == 0;
	if (am->bubble) {
		am->bubble_box = (Box){ am->x, y + 8, l->right_w,
			l->base + l->small + 28 };
		y = am->bubble_box.y + am->bubble_box.h + 4;
	}
	am->list_top = y + 4;
	build_todo(am, l->h - l->btn_h);
}

void
agenda_build(AgendaModel *am, const Geom *l)
{
	int64_t now = (int64_t)time(NULL);

	am->l = *l;
	am->x = l->left_w + l->gap;
	am->hover = ui.hover;
	build_controls(am);
	build_status(am, now);
	build_rows(am, now);
}

static int
check_click(int id)
{
	if (id < HIT_CHECK || id >= HIT_CHECK + ROWS_MAX ||
			!row_ref[id - HIT_CHECK][0])
		return 0;
	todo_done(row_ref[id - HIT_CHECK]);
	return 1;
}

int
agenda_click(int id)
{
	switch (id) {
	case HIT_GEAR:
	case HIT_BUBBLE:
		calendar_view(VIEW_ACCOUNTS);
		return 1;
	case HIT_TODO:
		agenda_todo_begin();
		return 1;
	case HIT_TARGET:
		todo_target_next();
		return 1;
	default:
		return check_click(id);
	}
}

void
agenda_scroll(int steps)
{
	ui.skip = MAX(ui.skip - steps, 0);
}

void
agenda_todo_begin(void)
{
	entry_begin(FIELD_TODO, "To-do", "");
}
