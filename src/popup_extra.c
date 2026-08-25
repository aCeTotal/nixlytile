/* Hover popups for the clock, volume, mic and light statusbar modules,
 * built on the shared popup card renderer.  Volume/mic popups are
 * interactive: pick the default sink/source and drag the gauge to set
 * the level. */
#include "nixlytile.h"
#include "popup_card.h"

#define AUDIO_DEV_MAX  8
#define SLIDER_HIT_ID  100
#define MUTE_HIT_ID    101

/* ── audio device cache (wpctl status is a fork — cache 2s) ──────── */

static AudioDevice sink_devs[AUDIO_DEV_MAX], src_devs[AUDIO_DEV_MAX];
static int sink_dev_count, src_dev_count;
static uint64_t sink_devs_ms, src_devs_ms;

static void
fetch_audio_devices(int sources)
{
	uint64_t now = monotonic_msec();
	AudioDevice *devs = sources ? src_devs : sink_devs;
	int *count = sources ? &src_dev_count : &sink_dev_count;
	uint64_t *ms = sources ? &src_devs_ms : &sink_devs_ms;

	if (*ms && now >= *ms && now - *ms < 2000)
		return;
	*count = audio_list_devices(sources, devs, AUDIO_DEV_MAX);
	*ms = now;
}

/* ── gauge slider drag state ─────────────────────────────────────── */

static struct {
	int active;        /* 0 none, 1 volume, 2 mic, 3 brightness */
	Monitor *mon;
	double frac;
	uint64_t last_set_ms;
} sdrag;

static InfoPopup *
sdrag_popup(void)
{
	if (!sdrag.active || !sdrag.mon)
		return NULL;
	return sdrag.active == 1 ? &sdrag.mon->statusbar.volume_popup :
		sdrag.active == 2 ? &sdrag.mon->statusbar.mic_popup :
		&sdrag.mon->statusbar.light_popup;
}

/* Push the dragged fraction to PipeWire + bar module. */
static void
slider_commit(void)
{
	double pct = sdrag.frac * 100.0;
	uint64_t now = monotonic_msec();

	if (sdrag.active == 1) {
		int is_headset = pipewire_sink_is_headset();

		if (volume_muted == 1)
			set_pipewire_mute(0);
		set_pipewire_volume(pct);
		volume_cache_store(is_headset, pct, 0, now);
		speaker_active = pct;
		refreshstatusvolume();
	} else if (sdrag.active == 2) {
		if (mic_muted == 1)
			set_pipewire_mic_mute(0);
		set_pipewire_mic_volume(pct);
		mic_last_percent = pct;
		mic_cached = pct;
		mic_cached_muted = 0;
		mic_muted = 0;
		mic_last_read_ms = now;
		microphone_active = pct;
		refreshstatusmic();
	} else if (sdrag.active == 3) {
		set_backlight_percent(pct);
		refreshstatuslight();
	}
}

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

/* Shared body for the volume/mic popups: header + draggable gauge +
 * state row + default-device picker. */
static void
render_audio_popup(Monitor *m, InfoPopup *p, int is_mic)
{
	Card *card;
	CardResult res;
	int is_headset = -1;
	double vol;
	char value[16];
	AudioDevice *devs = is_mic ? src_devs : sink_devs;
	int dev_count = is_mic ? src_dev_count : sink_dev_count;
	int muted = is_mic ? mic_muted : volume_muted;

	if (!p->tree)
		return;
	fetch_audio_devices(is_mic);
	dev_count = is_mic ? src_dev_count : sink_dev_count;

	if (sdrag.active && sdrag_popup() == p)
		vol = sdrag.frac * 100.0;
	else if (is_mic)
		vol = pipewire_mic_volume_percent();
	else
		vol = pipewire_volume_percent(&is_headset);

	card = card_begin();
	if (!card)
		return;

	if (vol >= 0.0)
		snprintf(value, sizeof(value), "%.0f%%", vol);
	else
		snprintf(value, sizeof(value), "--");
	if (is_mic)
		card_header(card, mic_icon_path, "Microphone", "AUDIO INPUT",
				value);
	else
		card_header(card, volume_icon_path, "Volume",
				is_headset == 1 ? "HEADSET" : "SPEAKERS", value);
	card_gap(card, 6);
	card_gauge_id(card, vol >= 0.0 ? vol / 100.0 : 0.0,
			muted ? card_col_red :
			(is_mic ? card_col_green : card_col_blue),
			SLIDER_HIT_ID);
	card_gap(card, 6);

	if (is_mic)
		card_kv2_btn(card, "Input level", value, NULL, "State",
				muted ? "Muted" : "Live",
				muted ? card_col_red : card_col_green,
				MUTE_HIT_ID, p->btn_hover == MUTE_HIT_ID);
	else
		card_kv2_btn(card, "Output", is_headset == 1 ? "Headset" : "Speakers",
				NULL, "State", muted ? "Muted" : "Active",
				muted ? card_col_red : card_col_green,
				MUTE_HIT_ID, p->btn_hover == MUTE_HIT_ID);

	if (dev_count > 0) {
		card_section(card, is_mic ? "INPUT DEVICE" : "OUTPUT DEVICE");
		for (int i = 0; i < dev_count; i++) {
			AudioDevice *d = &devs[i];
			char name[36];

			snprintf(name, sizeof(name), "%.33s", d->name);
			if (d->is_default)
				card_text(card, name, "Active", card_col_green);
			else
				card_text_btn(card, name, NULL, NULL, "Use",
						i, p->btn_hover == i);
		}
	}

	if (card_finish(card, &res) != 0)
		return;
	memcpy(p->hits, res.hits, sizeof(p->hits));
	p->nhits = res.nhits;
	popup_view_apply(&p->view, p->tree, &res);
	p->width = p->view.w;
	p->height = p->view.h;
}

static void
render_volume_popup(Monitor *m)
{
	render_audio_popup(m, &m->statusbar.volume_popup, 0);
}

static void
render_mic_popup(Monitor *m)
{
	render_audio_popup(m, &m->statusbar.mic_popup, 1);
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
	if (sdrag.active == 3 && sdrag_popup() == p)
		b = sdrag.frac * 100.0;
	else
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
	card_gauge_id(card, b >= 0.0 ? b / 100.0 : 0.0, card_col_yellow,
			SLIDER_HIT_ID);
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
	memcpy(p->hits, res.hits, sizeof(p->hits));
	p->nhits = res.nhits;
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

	/* an active gauge drag holds its popup open even when the cursor
	 * leaves the card */
	if (sdrag_popup() == p)
		inside = 1;

	was_visible = p->visible;

	/* hover highlight for device "Use" buttons */
	if (inside && p->visible && p->nhits > 0) {
		int rel_x = lx - popup_x;
		int rel_y = ly - m->statusbar.area.height;
		int nh = -1;

		for (int i = 0; i < p->nhits; i++) {
			CardHit *hit = &p->hits[i];

			if (hit->id == SLIDER_HIT_ID || hit->w <= 0)
				continue;
			if (rel_x >= hit->x && rel_x < hit->x + hit->w &&
					rel_y >= hit->y &&
					rel_y < hit->y + hit->h) {
				nh = hit->id;
				break;
			}
		}
		if (nh != p->btn_hover) {
			p->btn_hover = nh;
			p->last_render_ms = 0;
		}
	}

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
		p->btn_hover = -1;
		popup_view_hide(&p->view);
		wlr_scene_node_set_enabled(&p->tree->node, 0);
	}
}

/* Clamp like info_popup_hover does so click/drag coordinates line up
 * with where the popup is actually drawn. */
static int
info_popup_clamped_x(Monitor *m, StatusModule *mod, InfoPopup *p)
{
	int popup_x = mod->x;

	if (p->width > 0 && m->statusbar.area.width > 0) {
		int max_x = m->statusbar.area.width - p->width;

		if (max_x < 0)
			max_x = 0;
		if (popup_x > max_x)
			popup_x = max_x;
		if (popup_x < 0)
			popup_x = 0;
	}
	return popup_x;
}

static CardHit *
info_popup_slider_hit(InfoPopup *p)
{
	for (int i = 0; i < p->nhits; i++)
		if (p->hits[i].id == SLIDER_HIT_ID && p->hits[i].w > 0)
			return &p->hits[i];
	return NULL;
}

/* Ongoing gauge drag: update fraction from the cursor, ease the fill,
 * and push to PipeWire at most every 60 ms. */
static void
slider_drag_motion(Monitor *m, double cx)
{
	InfoPopup *p = sdrag_popup();
	StatusModule *mod;
	CardHit *track;
	int popup_x, rel_x;
	double frac;
	uint64_t now;

	if (!p || sdrag.mon != m)
		return;
	mod = sdrag.active == 1 ? &m->statusbar.volume :
		sdrag.active == 2 ? &m->statusbar.mic : &m->statusbar.light;
	track = info_popup_slider_hit(p);
	if (!track)
		return;

	popup_x = info_popup_clamped_x(m, mod, p);
	rel_x = (int)floor(cx) - m->statusbar.area.x - popup_x;
	frac = (double)(rel_x - track->x) / track->w;
	if (frac < 0.0)
		frac = 0.0;
	if (frac > 1.0)
		frac = 1.0;
	if (frac == sdrag.frac)
		return;
	sdrag.frac = frac;
	popup_view_set_fill_frac(&p->view, 0, frac);

	now = monotonic_msec();
	if (now - sdrag.last_set_ms >= 60) {
		sdrag.last_set_ms = now;
		slider_commit();
	}
}

/* Click in the volume/mic popup: gauge = start drag + set level,
 * device row "Use" = switch default sink/source. */
static int
audio_popup_click(Monitor *m, StatusModule *mod, InfoPopup *p, int is_mic,
		int lx, int ly, uint32_t button)
{
	AudioDevice *devs = is_mic ? src_devs : sink_devs;
	int count = is_mic ? src_dev_count : sink_dev_count;
	int popup_x, rel_x, rel_y;

	if (!p->visible || button != BTN_LEFT)
		return 0;

	popup_x = info_popup_clamped_x(m, mod, p);
	rel_x = lx - popup_x;
	rel_y = ly - m->statusbar.area.height;
	if (rel_x < 0 || rel_y < 0 || rel_x >= p->width || rel_y >= p->height)
		return 0;

	for (int i = 0; i < p->nhits; i++) {
		CardHit *hit = &p->hits[i];

		if (hit->w <= 0 ||
				rel_x < hit->x || rel_x >= hit->x + hit->w ||
				rel_y < hit->y || rel_y >= hit->y + hit->h)
			continue;
		if (hit->id == SLIDER_HIT_ID) {
			double frac = (double)(rel_x - hit->x) / hit->w;

			if (frac < 0.0)
				frac = 0.0;
			if (frac > 1.0)
				frac = 1.0;
			sdrag.active = is_mic ? 2 : 1;
			sdrag.mon = m;
			sdrag.frac = frac;
			sdrag.last_set_ms = monotonic_msec();
			popup_view_set_fill_frac(&p->view, 0, frac);
			slider_commit();
			return 1;
		}
		if (hit->id == MUTE_HIT_ID) {
			if (is_mic)
				toggle_pipewire_mic_mute();
			else
				toggle_pipewire_mute();
			render_audio_popup(m, p, is_mic);
			p->last_render_ms = monotonic_msec();
			return 1;
		}
		if (hit->id >= 0 && hit->id < count) {
			audio_set_default(devs[hit->id].id);
			if (is_mic) {
				src_devs_ms = 0;
				mic_last_read_ms = 0;
				refreshstatusmic();
			} else {
				sink_devs_ms = 0;
				volume_invalidate_cache(0);
				volume_invalidate_cache(1);
				refreshstatusvolume();
			}
			p->last_render_ms = 0;
			return 1;
		}
	}
	/* swallow clicks on the card body */
	return 1;
}

int
volume_popup_handle_click(Monitor *m, int lx, int ly, uint32_t button)
{
	return audio_popup_click(m, &m->statusbar.volume,
			&m->statusbar.volume_popup, 0, lx, ly, button);
}

int
mic_popup_handle_click(Monitor *m, int lx, int ly, uint32_t button)
{
	return audio_popup_click(m, &m->statusbar.mic,
			&m->statusbar.mic_popup, 1, lx, ly, button);
}

/* Click in the brightness popup: gauge = start drag + set level. */
int
light_popup_handle_click(Monitor *m, int lx, int ly, uint32_t button)
{
	InfoPopup *p = &m->statusbar.light_popup;
	CardHit *hit;
	int popup_x, rel_x, rel_y;

	if (!p->visible || button != BTN_LEFT)
		return 0;

	popup_x = info_popup_clamped_x(m, &m->statusbar.light, p);
	rel_x = lx - popup_x;
	rel_y = ly - m->statusbar.area.height;
	if (rel_x < 0 || rel_y < 0 || rel_x >= p->width || rel_y >= p->height)
		return 0;

	hit = info_popup_slider_hit(p);
	if (hit && rel_x >= hit->x && rel_x < hit->x + hit->w &&
			rel_y >= hit->y && rel_y < hit->y + hit->h) {
		double frac = (double)(rel_x - hit->x) / hit->w;

		if (frac < 0.0)
			frac = 0.0;
		if (frac > 1.0)
			frac = 1.0;
		sdrag.active = 3;
		sdrag.mon = m;
		sdrag.frac = frac;
		sdrag.last_set_ms = monotonic_msec();
		popup_view_set_fill_frac(&p->view, 0, frac);
		slider_commit();
	}
	/* swallow clicks on the card body */
	return 1;
}

void
info_popup_slider_release(void)
{
	InfoPopup *p = sdrag_popup();

	if (!p)
		return;
	slider_commit();
	p->last_render_ms = 0;   /* refresh the % text with the final level */
	sdrag.active = 0;
	sdrag.mon = NULL;
}

void
updateinfopopups(Monitor *m, double cx, double cy)
{
	if (!m)
		return;
	if (sdrag.active)
		slider_drag_motion(m, cx);
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
		p->btn_hover = -1;
		popup_view_hide(&p->view);
		if (p->tree)
			wlr_scene_node_set_enabled(&p->tree->node, 0);
	}
	if (sdrag.mon == m) {
		sdrag.active = 0;
		sdrag.mon = NULL;
	}
}
