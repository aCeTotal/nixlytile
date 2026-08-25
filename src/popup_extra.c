/* Hover popups for the clock, volume, mic and light statusbar modules,
 * built on the shared popup card renderer. */
#include "nixlytile.h"
#include "popup_card.h"

/* ── content builders ────────────────────────────────────────────── */

static void
render_clock_popup(Monitor *m)
{
	InfoPopup *p = &m->statusbar.clock_popup;
	Card *card;
	CardResult res;
	time_t t = time(NULL);
	struct tm lt;
	char value[16], sub[48], buf[64], v1[48];
	size_t i;

	if (!p->tree || !localtime_r(&t, &lt))
		return;

	card = card_begin();
	if (!card)
		return;

	strftime(value, sizeof(value), "%H:%M", &lt);
	strftime(sub, sizeof(sub), "%A", &lt);
	for (i = 0; sub[i]; i++)
		sub[i] = (char)toupper((unsigned char)sub[i]);
	card_header(card, clock_icon_path, "Clock", sub, value);
	card_gap(card, 6);

	strftime(v1, sizeof(v1), "%d %B %Y", &lt);
	card_kv2(card, "Date", v1, NULL, NULL, NULL, NULL);
	strftime(v1, sizeof(v1), "%V", &lt);
	{
		FILE *fp = fopen("/proc/uptime", "r");
		double up = -1.0;

		if (fp) {
			if (fscanf(fp, "%lf", &up) != 1)
				up = -1.0;
			fclose(fp);
		}
		if (up >= 0.0)
			snprintf(buf, sizeof(buf), "%dh %dm",
					(int)(up / 3600.0),
					(int)((up - (int)(up / 3600.0) * 3600.0) / 60.0));
		else
			snprintf(buf, sizeof(buf), "--");
	}
	card_kv2(card, "Week", v1, NULL, "Uptime", buf, NULL);

	strftime(buf, sizeof(buf), "%B %Y", &lt);
	for (i = 0; buf[i]; i++)
		buf[i] = (char)toupper((unsigned char)buf[i]);
	card_section(card, buf);
	card_calendar(card, lt.tm_year + 1900, lt.tm_mon, lt.tm_mday);

	if (card_finish(card, &res) != 0)
		return;
	popup_view_apply(&p->view, p->tree, &res);
	p->width = p->view.w;
	p->height = p->view.h;
}

static void
render_volume_popup(Monitor *m)
{
	InfoPopup *p = &m->statusbar.volume_popup;
	Card *card;
	CardResult res;
	int is_headset = -1;
	double vol;
	char value[16];

	if (!p->tree)
		return;
	vol = pipewire_volume_percent(&is_headset);

	card = card_begin();
	if (!card)
		return;

	if (vol >= 0.0)
		snprintf(value, sizeof(value), "%.0f%%", vol);
	else
		snprintf(value, sizeof(value), "--");
	card_header(card, volume_icon_path, "Volume",
			is_headset == 1 ? "HEADSET" : "SPEAKERS", value);
	card_gap(card, 6);
	card_gauge(card, vol >= 0.0 ? vol / 100.0 : 0.0,
			volume_muted ? card_col_red : card_col_blue);
	card_gap(card, 6);

	card_kv2(card, "Output", is_headset == 1 ? "Headset" : "Speakers",
			NULL, "State", volume_muted ? "Muted" : "Active",
			volume_muted ? card_col_red : card_col_green);

	if (card_finish(card, &res) != 0)
		return;
	popup_view_apply(&p->view, p->tree, &res);
	p->width = p->view.w;
	p->height = p->view.h;
}

static void
render_mic_popup(Monitor *m)
{
	InfoPopup *p = &m->statusbar.mic_popup;
	Card *card;
	CardResult res;
	double vol;
	char value[16];

	if (!p->tree)
		return;
	vol = pipewire_mic_volume_percent();

	card = card_begin();
	if (!card)
		return;

	if (vol >= 0.0)
		snprintf(value, sizeof(value), "%.0f%%", vol);
	else
		snprintf(value, sizeof(value), "--");
	card_header(card, mic_icon_path, "Microphone", "AUDIO INPUT", value);
	card_gap(card, 6);
	card_gauge(card, vol >= 0.0 ? vol / 100.0 : 0.0,
			mic_muted ? card_col_red : card_col_green);
	card_gap(card, 6);

	card_kv2(card, "Input level", value, NULL, "State",
			mic_muted ? "Muted" : "Live",
			mic_muted ? card_col_red : card_col_green);

	if (card_finish(card, &res) != 0)
		return;
	popup_view_apply(&p->view, p->tree, &res);
	p->width = p->view.w;
	p->height = p->view.h;
}

static void
render_light_popup(Monitor *m)
{
	InfoPopup *p = &m->statusbar.light_popup;
	Card *card;
	CardResult res;
	double b;
	char value[16], dev[64];

	if (!p->tree)
		return;
	b = backlight_percent();

	card = card_begin();
	if (!card)
		return;

	if (b >= 0.0)
		snprintf(value, sizeof(value), "%.0f%%", b);
	else
		snprintf(value, sizeof(value), "--");
	card_header(card, light_icon_path, "Brightness", "BACKLIGHT", value);
	card_gap(card, 6);
	card_gauge(card, b >= 0.0 ? b / 100.0 : 0.0, card_col_yellow);
	card_gap(card, 6);

	/* .../backlight/<device>/brightness → <device> */
	dev[0] = '\0';
	if (backlight_brightness_path[0]) {
		char tmp[PATH_MAX];
		char *slash;

		snprintf(tmp, sizeof(tmp), "%s", backlight_brightness_path);
		slash = strrchr(tmp, '/');
		if (slash) {
			*slash = '\0';
			slash = strrchr(tmp, '/');
			if (slash && slash[1])
				snprintf(dev, sizeof(dev), "%s", slash + 1);
		}
	}
	card_kv2(card, "Device", dev[0] ? dev : "--", NULL, NULL, NULL, NULL);

	if (card_finish(card, &res) != 0)
		return;
	popup_view_apply(&p->view, p->tree, &res);
	p->width = p->view.w;
	p->height = p->view.h;
}

/* ── generic hover plumbing ──────────────────────────────────────── */

/* Re-render cadence while visible; volume/light react to scroll wheel
 * changes on the module underneath the cursor. */
#define INFO_RERENDER_MS 500

static void
info_popup_hover(Monitor *m, StatusModule *mod, InfoPopup *p,
		void (*render)(Monitor *), double cx, double cy)
{
	int lx, ly, inside = 0, was_visible;
	int popup_x;
	uint64_t now = monotonic_msec();

	if (!m || !m->showbar || !mod->tree || !p->tree || mod->width <= 0) {
		if (p->tree && p->visible) {
			wlr_scene_node_set_enabled(&p->tree->node, 0);
			p->visible = 0;
			p->hover_start_ms = 0;
			popup_view_hide(&p->view);
		}
		return;
	}

	lx = (int)floor(cx) - m->statusbar.area.x;
	ly = (int)floor(cy) - m->statusbar.area.y;

	popup_x = mod->x;
	if (p->width > 0 && m->statusbar.area.width > 0) {
		int max_x = m->statusbar.area.width - p->width;

		if (max_x < 0)
			max_x = 0;
		if (popup_x > max_x)
			popup_x = max_x;
		if (popup_x < 0)
			popup_x = 0;
	}

	if (lx >= mod->x && lx < mod->x + mod->width &&
			ly >= 0 && ly < m->statusbar.area.height)
		inside = 1;
	else if (p->visible && p->width > 0 && p->height > 0 &&
			lx >= popup_x && lx < popup_x + p->width &&
			ly >= m->statusbar.area.height &&
			ly < m->statusbar.area.height + p->height)
		inside = 1;

	was_visible = p->visible;

	if (inside) {
		if (p->hover_start_ms == 0)
			p->hover_start_ms = now;
		if (!was_visible && (now - p->hover_start_ms) < 300) {
			uint64_t remaining = 300 - (now - p->hover_start_ms);

			schedule_popup_delay(remaining + 1);
			return;
		}

		p->visible = 1;
		if (!was_visible || p->last_render_ms == 0 ||
				now < p->last_render_ms ||
				now - p->last_render_ms >= INFO_RERENDER_MS) {
			render(m);
			p->last_render_ms = now;
			/* re-clamp with the fresh size */
			popup_x = mod->x;
			if (p->width > 0 && m->statusbar.area.width > 0) {
				int max_x = m->statusbar.area.width - p->width;

				if (max_x < 0)
					max_x = 0;
				if (popup_x > max_x)
					popup_x = max_x;
				if (popup_x < 0)
					popup_x = 0;
			}
		}
		wlr_scene_node_set_enabled(&p->tree->node, 1);
		wlr_scene_node_set_position(&p->tree->node,
				popup_x, m->statusbar.area.height);
		if (!was_visible)
			popup_view_show(&p->view);
	} else if (p->visible || p->hover_start_ms != 0) {
		p->visible = 0;
		p->hover_start_ms = 0;
		p->last_render_ms = 0;
		popup_view_hide(&p->view);
		wlr_scene_node_set_enabled(&p->tree->node, 0);
	}
}

void
updateinfopopups(Monitor *m, double cx, double cy)
{
	if (!m)
		return;
	info_popup_hover(m, &m->statusbar.clock, &m->statusbar.clock_popup,
			render_clock_popup, cx, cy);
	info_popup_hover(m, &m->statusbar.volume, &m->statusbar.volume_popup,
			render_volume_popup, cx, cy);
	info_popup_hover(m, &m->statusbar.mic, &m->statusbar.mic_popup,
			render_mic_popup, cx, cy);
	info_popup_hover(m, &m->statusbar.light, &m->statusbar.light_popup,
			render_light_popup, cx, cy);
}

/* 1 while some info popup is waiting out its show delay — the shared
 * popup-delay timer uses this to know it must re-poll. */
int
info_popup_pending(Monitor *m)
{
	return (m->statusbar.clock_popup.hover_start_ms != 0 &&
			!m->statusbar.clock_popup.visible) ||
		(m->statusbar.volume_popup.hover_start_ms != 0 &&
			!m->statusbar.volume_popup.visible) ||
		(m->statusbar.mic_popup.hover_start_ms != 0 &&
			!m->statusbar.mic_popup.visible) ||
		(m->statusbar.light_popup.hover_start_ms != 0 &&
			!m->statusbar.light_popup.visible);
}

int
info_popup_visible(Monitor *m)
{
	return m->statusbar.clock_popup.visible ||
		m->statusbar.volume_popup.visible ||
		m->statusbar.mic_popup.visible ||
		m->statusbar.light_popup.visible;
}

void
info_popups_hide(Monitor *m)
{
	InfoPopup *ps[4] = { &m->statusbar.clock_popup,
		&m->statusbar.volume_popup, &m->statusbar.mic_popup,
		&m->statusbar.light_popup };

	for (int i = 0; i < 4; i++) {
		InfoPopup *p = ps[i];

		p->visible = 0;
		p->hover_start_ms = 0;
		p->last_render_ms = 0;
		popup_view_hide(&p->view);
		if (p->tree)
			wlr_scene_node_set_enabled(&p->tree->node, 0);
	}
}
