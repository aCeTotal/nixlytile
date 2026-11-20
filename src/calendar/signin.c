/* Gear view actions: sign-in, removal. */
#include "calendar/view.h"

#define PAYLOAD_LEN (FORM_SLOTS * (TEXT_ENTRY_MAX + sizeof("user=\n")))

void
accounts_commit(void)
{
	if (ui.field < FIELD_FORM || !entry_ours())
		return;
	snprintf(ui.form[ui.field - FIELD_FORM], TEXT_ENTRY_MAX, "%s",
			text_entry_text());
}

static void
form_reset(void)
{
	if (ui.field >= FIELD_FORM && entry_ours())
		text_entry_cancel();
	explicit_bzero(ui.form, sizeof(ui.form));
	ui.form_error[0] = '\0';
}

static int
next_field(const Provider *p, int from)
{
	for (int f = from; f < FORM_SLOTS; f++)
		if (p->fields[f] && !ui.form[f][0])
			return f;
	return -1;
}

/* Payload lines stay single. */
static int
form_clean(void)
{
	for (int f = 0; f < FORM_SLOTS; f++)
		if (strpbrk(ui.form[f], "\r\n"))
			return 0;
	return 1;
}

static void
account_connect(void)
{
	const Provider *p = provider_effective(ui.provider);
	char payload[PAYLOAD_LEN];
	int missing;

	accounts_commit();
	if (ui.field >= FIELD_FORM && entry_ours())
		text_entry_cancel();
	if (job_busy(JOB_ADD) || !form_clean())
		return;
	missing = p->oauth ? -1 : next_field(p, 0);
	if (missing >= 0) {
		snprintf(ui.form_error, sizeof(ui.form_error), "%s is missing",
				p->fields[missing]);
		return;
	}
	if (p->oauth)
		snprintf(payload, sizeof(payload),
				"client_id=%s\nclient_secret=%s\n", p->client_id,
				p->client_secret);
	else
		snprintf(payload, sizeof(payload), "url=%s\nuser=%s\npass=%s\n",
				ui.form[FORM_URL], ui.form[FORM_USER],
				ui.form[FORM_SECRET]);
	ui.form_error[0] = '\0';
	ui.adding = ui.provider;
	if (job_spawn(&(JobSpec){ .kind = JOB_ADD, .cmd = "add",
			.arg = p->id, .payload = payload }) == 0)
		spin_start();
	explicit_bzero(payload, sizeof(payload));
	explicit_bzero(ui.form[FORM_SECRET], sizeof(ui.form[FORM_SECRET]));
}

static void
provider_click(int i)
{
	const Provider *p = provider_effective(i);
	int f;

	if (job_busy(JOB_ADD))
		return;
	form_reset();
	ui.provider = ui.provider == i ? -1 : i;
	if (ui.provider < 0)
		return;
	if (p->oauth) {
		account_connect();
		return;
	}
	f = next_field(p, 0);
	if (f >= 0)
		entry_begin(FIELD_FORM + f, p->fields[f], "");
}

static void
remove_click(int i)
{
	const char *id = store_account(i)->id;

	if (strcmp(ui.remove_id, id)) {
		snprintf(ui.remove_id, sizeof(ui.remove_id), "%s", id);
		return;
	}
	job_spawn(&(JobSpec){ .kind = JOB_REMOVE, .cmd = "remove", .arg = id });
	ui.remove_id[0] = '\0';
}

static void
field_click(int f)
{
	accounts_commit();
	entry_begin(FIELD_FORM + f, provider_effective(ui.provider)->fields[f],
			ui.form[f]);
}

static int
range_click(int id)
{
	if (id >= HIT_REMOVE && id < HIT_REMOVE + store_account_count()) {
		remove_click(id - HIT_REMOVE);
		return 1;
	}
	if (id >= HIT_PROVIDER && id < HIT_PROVIDER + PROVIDERS) {
		provider_click(id - HIT_PROVIDER);
		return 1;
	}
	if (id < HIT_FIELD || id >= HIT_FIELD + FORM_SLOTS || ui.provider < 0)
		return 0;
	field_click(id - HIT_FIELD);
	return 1;
}

int
accounts_click(int id)
{
	switch (id) {
	case HIT_BACK:
		form_reset();
		ui.provider = -1;
		calendar_view(VIEW_MONTH);
		return 1;
	case HIT_CONNECT:
		account_connect();
		return 1;
	case HIT_CANCEL:
		job_cancel(JOB_ADD);
		return 1;
	default:
		return range_click(id);
	}
}

/* Enter moves on, then connects. */
void
accounts_submit(Field f, const char *text)
{
	const Provider *p = provider_effective(ui.provider);
	int next;

	snprintf(ui.form[f - FIELD_FORM], TEXT_ENTRY_MAX, "%s", text);
	next = next_field(p, f - FIELD_FORM + 1);
	if (next >= 0) {
		entry_begin(FIELD_FORM + next, p->fields[next], "");
		return;
	}
	account_connect();
}

void
calendar_account_added(const char *label)
{
	char msg[MSG_LEN];

	snprintf(msg, sizeof(msg), "Calendar connected · %s", label);
	osd_show(selmon, msg);
	form_reset();
	ui.provider = -1;
}

void
calendar_add_failed(const char *error)
{
	snprintf(ui.form_error, sizeof(ui.form_error), "%s", error);
}
