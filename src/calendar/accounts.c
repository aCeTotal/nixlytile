/* Gear view: accounts and providers. */
#include "calendar/view.h"

#define NOTES_MAX   6
#define SPIN_PERIOD 1000.0
#define DOTS_MAX    64
#define SUB_LEN     64

typedef struct {
	char text[LINE_LEN];
	const float *col;
} Note;

static Note notes[NOTES_MAX];
static int nnotes, view_w;

static void
note_text(CardInk *ink, const void *model)
{
	const Note *n = model;

	card_ink_text(ink, &(CardText){ CARD_FONT_SMALL, n->text, 0, 2,
			n->col });
}

static const CardPainter note_painter = { NULL, note_text };

static void
note(Card *card, const char *text, const float *col)
{
	Note *n = &notes[nnotes++ % NOTES_MAX];

	memset(n, 0, sizeof(*n));
	fit_text(n->text, sizeof(n->text), (FitSpec){ text, CARD_FONT_SMALL,
			view_w });
	n->col = col;
	card_custom(card, &(CardBlock){ &note_painter, n, sizeof(*n), view_w,
			card_font_height(CARD_FONT_SMALL) + 6 });
}

static void
render_account(Card *card, int i)
{
	const CalAccount *a = store_account(i);
	const AccountState *s = account_state(a->id);
	const Provider *p = provider_by_id(a->provider);
	int failed = s && s->error[0], synced = s && s->ok_at;
	char line[LINE_LEN], sub[SUB_LEN];

	card_icon_text_rbtn(card, p ? p->icon : "cal-server", a->label,
			failed ? "Error" : synced ? "Synced" : "Waiting",
			failed ? card_col_red : synced ? card_col_green :
			card_col_dim,
			strcmp(ui.remove_id, a->id) ? "Remove" : "Confirm",
			HIT_REMOVE + i, ui.hover == HIT_REMOVE + i);
	snprintf(line, sizeof(line), "%s · %d calendar%s",
			provider_name(a->provider), a->ncals, a->ncals == 1 ? "" : "s");
	fit_text(sub, sizeof(sub), (FitSpec){ failed ? s->error : line,
			CARD_FONT_SMALL, view_w / 2 });
	card_row_sub(card, sub, failed ? card_col_red : NULL);
}

static void
render_field(Card *card, const Provider *p, int f)
{
	char shown[LINE_LEN], dots[3 * DOTS_MAX + 1] = "";
	const char *value = ui.form[f];
	int editing = entry_on(FIELD_FORM + f);

	for (size_t i = 0; f == FORM_SECRET && value[i] && i < DOTS_MAX; i++)
		strcat(dots, "•");
	if (editing)
		value = text_entry_display();
	else if (f == FORM_SECRET)
		value = dots;
	fit_tail(shown, sizeof(shown), (FitSpec){ value[0] ? value :
			"Click to type", CARD_FONT_BASE, view_w / 2 });
	card_icon_text_hit(card, NULL, p->fields[f], shown,
			editing ? card_col_blue : value[0] ? card_col_fg :
			card_col_faint, NULL, HIT_FIELD + f,
			ui.hover == HIT_FIELD + f);
}

static void
render_busy(Card *card)
{
	const Provider *p = provider_effective(ui.adding);

	card_loading(card, p->oauth ? "Waiting for sign-in in your browser…" :
			"Connecting…", (double)monotonic_msec() / SPIN_PERIOD);
	card_text_rbtn(card, "", NULL, NULL, "Cancel", HIT_CANCEL,
			ui.hover == HIT_CANCEL);
}

static void
render_form(Card *card, const Provider *p)
{
	const char *labels[1] = { p->oauth ? "Sign in with browser" : "Connect" };

	for (int f = 0; f < FORM_SLOTS; f++)
		if (p->fields[f])
			render_field(card, p, f);
	note(card, p->hint, card_col_dim);
	card_gap(card, 4);
	card_buttons(card, labels, NULL, 1, -1,
			ui.hover == HIT_CONNECT ? 0 : -1, HIT_CONNECT);
}

static void
render_section(Card *card)
{
	int busy = job_busy(JOB_ADD);
	int pick = busy ? ui.adding : ui.provider;
	char title[32];

	if (pick < 0 && !ui.form_error[0])
		return;
	snprintf(title, sizeof(title), "%s", pick < 0 ? "Sign-in" :
			provider_at(pick)->name);
	card_section(card, upcase(title));
	if (ui.form_error[0])
		note(card, ui.form_error, card_col_red);
	if (busy)
		render_busy(card);
	else if (pick >= 0)
		render_form(card, provider_effective(pick));
}

void
accounts_render(Card *card, const Geom *l)
{
	int n = store_account_count();
	char count[8] = "";

	view_w = l->left_w + l->gap + l->right_w;
	nnotes = 0;
	if (n)
		snprintf(count, sizeof(count), "%d", n);
	card_min_w(card, view_w);
	card_header(card, "cal-gear", "Calendar sync", "SYNCS EVERY MINUTE",
			count);
	card_gap(card, 4);
	card_text_rbtn(card, n ? "Connected accounts" : "No accounts yet",
			NULL, NULL, "Back", HIT_BACK, ui.hover == HIT_BACK);
	for (int i = 0; i < n; i++)
		render_account(card, i);
	card_section(card, "ADD ACCOUNT");
	provider_tiles(card, view_w);
	render_section(card);
}

