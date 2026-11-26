/* Statusbar update icon and popup. */
#include <sys/inotify.h>

#include "nixlytile.h"
#include "popup_card.h"

#define UPDATE_DIR        "/run/nixlyos"
#define UPDATE_LIST       UPDATE_DIR "/available"
#define UPDATE_RUNNING    UPDATE_DIR "/updating"
#define UPDATE_FAILED     UPDATE_DIR "/failed"
#define UPDATE_PROGRESS   UPDATE_DIR "/progress"
#define UPDATE_ROWS_MAX   1024
#define UPDATE_ROWS_SHOWN 18
#define UPDATE_SCROLL_ROWS 3
#define UPDATE_HIT_NOW    600
#define DASH              "\xe2\x80\x93"

typedef enum { ROW_UP, ROW_DOWN, ROW_NEW, ROW_GONE } RowKind;

typedef struct {
	RowKind kind;
	char name[64];
	char change[128];
} UpdateRow;

static const struct {
	const char *tag;
	RowKind kind;
} row_tags[] = {
	{ "up", ROW_UP }, { "down", ROW_DOWN },
	{ "new", ROW_NEW }, { "gone", ROW_GONE },
};

/* nixlyos-update phases, as reported. */
static const struct {
	const char *key;
	const char *label;
} phases[] = {
	{ "release", "Fetching release" }, { "eval", "Evaluating" },
	{ "fetch", "Checking cache" }, { "cache", "Checking cache" },
	{ "room", "Freeing space" }, { "download", "Downloading" },
	{ "build", "Building" }, { "activate", "Activating" },
};

static struct {
	UpdateRow rows[UPDATE_ROWS_MAX];
	int nrows;
	int deps;
	char release[48];
	char download[24];
	int running;
	const char *phase;
	int percent;
	char percent_text[16];
	char detail[96];
	int failed;
	int scroll;
} upd;

static int update_fd = -1;
static struct wl_event_source *update_source;

static const float *
row_color(RowKind kind)
{
	static const float *const colors[] = {
		[ROW_UP] = card_col_green, [ROW_DOWN] = card_col_yellow,
		[ROW_NEW] = card_col_blue, [ROW_GONE] = card_col_dim,
	};

	return colors[kind];
}

/* Next tab field, empty ones kept. */
static char *
field(char **rest)
{
	char *start = *rest;
	char *tab = strchr(start, '\t');

	*rest = tab ? tab + 1 : start + strlen(start);
	if (tab)
		*tab = '\0';
	return start;
}

/* One TSV line from nixlyos-check. */
static void
update_parse(char *line)
{
	char *tag = field(&line);
	char *a = field(&line);
	char *b = field(&line);
	char *c = field(&line);

	if (strcmp(tag, "release") == 0) {
		snprintf(upd.release, sizeof(upd.release), "%s \xc2\xb7 %s", a, b);
		return;
	}
	if (strcmp(tag, "deps") == 0) {
		upd.deps = atoi(a);
		return;
	}
	if (strcmp(tag, "download") == 0) {
		snprintf(upd.download, sizeof(upd.download), "%s", a);
		return;
	}
	for (size_t i = 0; i < LENGTH(row_tags); i++) {
		UpdateRow *row = &upd.rows[upd.nrows];

		if (strcmp(tag, row_tags[i].tag) != 0 || upd.nrows == UPDATE_ROWS_MAX)
			continue;
		row->kind = row_tags[i].kind;
		snprintf(row->name, sizeof(row->name), "%s", a);
		snprintf(row->change, sizeof(row->change), "%s > %s",
				*b ? b : DASH, *c ? c : DASH);
		upd.nrows++;
		return;
	}
}

/* "phase<TAB>percent<TAB>detail" from nixlyos-update. */
static void
progress_load(void)
{
	char line[256], *rest = line, *key;
	FILE *fp = fopen(UPDATE_PROGRESS, "r");

	upd.phase = "Starting";
	upd.percent = -1;
	upd.percent_text[0] = '\0';
	upd.detail[0] = '\0';
	if (!fp)
		return;
	if (!fgets(line, sizeof(line), fp)) {
		fclose(fp);
		return;
	}
	fclose(fp);
	line[strcspn(line, "\n")] = '\0';
	key = field(&rest);
	upd.percent = atoi(field(&rest));
	if (upd.percent >= 0)
		snprintf(upd.percent_text, sizeof(upd.percent_text), "%d%%", upd.percent);
	snprintf(upd.detail, sizeof(upd.detail), "%s", field(&rest));
	for (size_t i = 0; i < LENGTH(phases); i++)
		if (strcmp(key, phases[i].key) == 0)
			upd.phase = phases[i].label;
}

static void
update_load(void)
{
	char line[512];
	FILE *fp;

	upd.nrows = 0;
	upd.deps = 0;
	upd.release[0] = '\0';
	upd.download[0] = '\0';
	upd.running = access(UPDATE_RUNNING, F_OK) == 0;
	upd.failed = access(UPDATE_FAILED, F_OK) == 0;
	progress_load();
	fp = fopen(UPDATE_LIST, "r");
	if (!fp)
		return;
	while (fgets(line, sizeof(line), fp)) {
		line[strcspn(line, "\n")] = '\0';
		update_parse(line);
	}
	fclose(fp);
	upd.scroll = MIN(upd.scroll, MAX(0, upd.nrows - UPDATE_ROWS_SHOWN));
}

static int
update_shown(void)
{
	return upd.release[0] || upd.running || upd.failed;
}

void
renderupdate(StatusModule *module, int bar_height, const char *text)
{
	const float *fg = upd.failed ? statusbar_crit_fg :
		upd.running ? statusbar_warn_fg : statusbar_icon_fg;

	(void)text;
	if (!update_shown()) {
		bar_module_clear(module);
		return;
	}
	bar_module_draw(module, bar_height, &(BarLabel){
		.icon = GLYPH_UPDATE, .icon_fg = fg, .text_fg = fg,
		.text = upd.running && upd.percent_text[0] ? upd.percent_text : NULL });
}

/* Phase, gauge and detail, live. */
static void
update_progress(Card *card)
{
	card_text(card, upd.phase, upd.percent_text, card_col_blue);
	if (upd.percent >= 0)
		card_gauge(card, upd.percent / 100.0, card_col_blue);
	if (upd.detail[0])
		card_text(card, "", upd.detail, card_col_dim);
}

static void
update_button(Card *card, InfoPopup *p)
{
	if (upd.running) {
		update_progress(card);
		return;
	}
	card_big_btn(card, "Update now", card_col_blue, UPDATE_HIT_NOW,
			p->btn_hover == UPDATE_HIT_NOW);
}

void
render_update_popup(Monitor *m)
{
	InfoPopup *p = &m->statusbar.update_popup;
	int last = MIN(upd.nrows, upd.scroll + UPDATE_ROWS_SHOWN);
	char line[64];
	Card *card;
	CardResult res;

	if (!p->tree)
		return;
	card = card_begin();
	if (!card)
		return;
	card_at(m, m->statusbar.area.x + p->tree->node.x,
			m->statusbar.area.y + statusbar_popup_y(m));
	card_header(card, NULL, "Update", upd.release, upd.download);
	for (int i = upd.scroll; i < last; i++)
		card_text(card, upd.rows[i].name, upd.rows[i].change,
				row_color(upd.rows[i].kind));
	if (upd.nrows > UPDATE_ROWS_SHOWN) {
		snprintf(line, sizeof(line), "%d" DASH "%d of %d",
				upd.scroll + 1, last, upd.nrows);
		card_text(card, "Scroll for more", line, card_col_dim);
	}
	if (upd.deps) {
		snprintf(line, sizeof(line), "%d changed", upd.deps);
		card_text(card, "Dependencies", line, card_col_dim);
	}
	if (upd.failed)
		card_text(card, "Last update failed", "journalctl -u nixlyos-update",
				card_col_red);
	update_button(card, p);

	if (card_finish_sig(card, &res, &p->view.render_sig) != 0)
		return; /* Error, or unchanged. */
	memcpy(p->hits, res.hits, sizeof(p->hits));
	p->nhits = res.nhits;
	popup_view_apply(&p->view, p->tree, &res);
	p->width = p->view.w;
	p->height = p->view.h;
}

int
update_popup_handle_click(Monitor *m, int lx, int ly, uint32_t button)
{
	InfoPopup *p = &m->statusbar.update_popup;
	const char *const argv[] =
		{ "systemctl", "start", "--no-block", "nixlyos-update.service", NULL };
	int rel_x, rel_y;

	if (!p->visible || !p->tree || button != BTN_LEFT)
		return 0;
	rel_x = lx - p->tree->node.x;
	rel_y = ly - statusbar_popup_y(m);
	if (rel_x < 0 || rel_y < 0 || rel_x >= p->width || rel_y >= p->height)
		return 0;
	for (int i = 0; i < p->nhits; i++) {
		CardHit *hit = &p->hits[i];

		if (hit->id != UPDATE_HIT_NOW)
			continue;
		if (rel_x < hit->x || rel_x >= hit->x + hit->w ||
				rel_y < hit->y || rel_y >= hit->y + hit->h)
			continue;
		if (!upd.running)
			spawn_cmd_async(argv);
		return 1;
	}
	/* Card body swallows clicks. */
	return 1;
}

int
update_popup_handle_scroll(Monitor *m, int steps)
{
	upd.scroll = MAX(0, MIN(upd.scroll - steps * UPDATE_SCROLL_ROWS,
			upd.nrows - UPDATE_ROWS_SHOWN));
	render_update_popup(m);
	return 1;
}

void
refreshstatusupdate(void)
{
	Monitor *m;

	update_load();
	wl_list_for_each(m, &mons, link) {
		if (!m->showbar)
			continue;
		if (m->statusbar.update.tree)
			renderupdate(&m->statusbar.update,
					m->statusbar.area.height ? m->statusbar.area.height :
					(int)statusbar_height, "");
		if (m->statusbar.update_popup.visible)
			render_update_popup(m);
		positionstatusmodules(m);
	}
}

/* Dot files are writes in flight. */
static int
update_readable(int fd, uint32_t mask, void *data)
{
	char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
	int relevant = 0;
	ssize_t len;

	(void)mask;
	(void)data;
	while ((len = read(fd, buf, sizeof(buf))) > 0) {
		for (char *p = buf; p < buf + len;) {
			struct inotify_event *ev = (struct inotify_event *)p;

			if (ev->len && ev->name[0] != '.')
				relevant = 1;
			p += sizeof(*ev) + ev->len;
		}
	}
	if (relevant)
		refreshstatusupdate();
	return 0;
}

void
update_ui_setup(void)
{
	update_load();
	update_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (update_fd < 0) {
		nx_log(NX_ERROR, "update: inotify_init failed: %s", strerror(errno));
		return;
	}
	/* Missing outside NixlyOS. */
	if (inotify_add_watch(update_fd, UPDATE_DIR,
			IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE | IN_DELETE) < 0) {
		close(update_fd);
		update_fd = -1;
		return;
	}
	update_source = wl_event_loop_add_fd(event_loop, update_fd,
		WL_EVENT_READABLE, update_readable, NULL);
}

void
update_ui_cleanup(void)
{
	if (update_source) {
		wl_event_source_remove(update_source);
		update_source = NULL;
	}
	if (update_fd >= 0) {
		close(update_fd);
		update_fd = -1;
	}
}
