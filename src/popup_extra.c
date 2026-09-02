/* Hover popups for the clock, volume, mic and light statusbar modules,
 * built on the shared popup card renderer.  Volume/mic popups are
 * interactive: pick the default sink/source and drag the gauge to set
 * the level. */
#include "nixlytile.h"
#include "popup_card.h"
#include "fetch_async.h"

#define AUDIO_DEV_MAX  8
#define SLIDER_HIT_ID  100
#define MUTE_HIT_ID    101

#define METER_HIST     128
#define METER_TICK_MS  16   /* draw rate (~60 fps) */
#define METER_PUSH_MS  33   /* history sample period */
#define LIGHT_MODE_HIT_BASE 200
#define FAN_SLIDER_HIT  270  /* manual gauge of the expanded fan */
#define FAN_MODE_HIT    272  /* +0 Auto  +1 Manual  +2 Curve */
#define FAN_CURVE_HIT   276  /* curve plot of the expanded fan */
#define FAN_ROW_BASE    300  /* + flat fan index (expand/collapse) */
#define FAN_BOOST_HIT   380  /* msi-ec cooler boost chip */

/* ── live audio meter overlay (volume=0 / mic=1) ─────────────────── */

typedef struct {
	struct wlr_scene_buffer *node;   /* child of view content; reset on
	                                  * every re-render (children are
	                                  * destroyed by popup_view_apply) */
	int x, y, w, h;                  /* rect from CardResult */
	float hist[METER_HIST];
	int head;
	MeterRaster raster;              /* persistent ping-pong frame pair */
} MeterUI;
static MeterUI meter_ui[2];
static struct wl_event_source *meter_timer;
static float meter_pend;          /* max peak since the last history push */
static uint64_t meter_push_ms;    /* when the newest sample was pushed */

static int meter_tick(void *data);

/* Fraction of the sample period elapsed since the newest push — drives
 * the sub-pixel slide in card_meter_buffer.  May exceed 1.0: pushes are
 * quantized to timer ticks, so a late push would otherwise freeze the
 * slide at the slot boundary and then jump backwards when the sample
 * lands.  Letting the phase run on keeps the scroll uniform through a
 * late push (the timeline itself is kept uniform by the cadence-stable
 * meter_push_ms += METER_PUSH_MS in meter_tick). */
static double
meter_phase(uint64_t now)
{
	double ph;

	if (!meter_push_ms || now <= meter_push_ms)
		return 0.0;
	ph = (double)(now - meter_push_ms) / METER_PUSH_MS;
	return ph > 2.0 ? 2.0 : ph;
}

/* (Re)create the overlay node and swap in a freshly drawn frame. Safe
 * to call right after a card re-render so the meter never drops out for
 * a frame. */
static void
meter_overlay_draw(InfoPopup *p, int is_mic, double phase)
{
	MeterUI *mu = &meter_ui[is_mic];
	struct wlr_buffer *buf;
	int muted = is_mic ? mic_muted : volume_muted;

	/* Wait out the card show animation: extra children don't take part
	 * in its fade, so the meter joins once the card has settled. */
	if (mu->w <= 0 || !p->view.content || p->view.animating)
		return;
	if (!mu->node) {
		mu->node = wlr_scene_buffer_create(p->view.content, NULL);
		if (mu->node)
			wlr_scene_node_set_position(&mu->node->node,
					mu->x, mu->y);
	}
	/* Persistent ping-pong raster: no per-frame alloc/memcpy, and the
	 * returned buffer stays owned by mu->raster — no drop here. */
	buf = card_meter_raster(&mu->raster, mu->w, mu->h,
			muted == 1 ? card_col_red :
			(is_mic ? card_col_green : card_col_blue),
			mu->hist, METER_HIST, mu->head, phase);
	if (mu->node && buf)
		wlr_scene_buffer_set_buffer(mu->node, buf);
}

static void
meter_timer_arm(void)
{
	if (!meter_timer)
		meter_timer = wl_event_loop_add_timer(event_loop, meter_tick,
				NULL);
	if (meter_timer)
		wl_event_source_timer_update(meter_timer, METER_TICK_MS);
}

/* ── audio device cache (async wpctl status — cache 2s) ──────────── */

static AudioDevice sink_devs[AUDIO_DEV_MAX], src_devs[AUDIO_DEV_MAX];
static int sink_dev_count, src_dev_count;
static uint64_t sink_devs_ms, src_devs_ms;
static int devs_fetch_inflight;

/* One wpctl status answer fills both caches; re-render the open popup
 * so the device list appears without waiting for cursor motion. */
static void
devs_fetch_done(const char *out, size_t len, void *data)
{
	FILE *fp;

	(void)data;
	devs_fetch_inflight = 0;
	if (!len)
		return;
	fp = fmemopen((void *)out, len, "r");
	if (fp) {
		sink_dev_count = audio_parse_status_devices(fp, 0, sink_devs,
				AUDIO_DEV_MAX);
		fclose(fp);
	}
	fp = fmemopen((void *)out, len, "r");
	if (fp) {
		src_dev_count = audio_parse_status_devices(fp, 1, src_devs,
				AUDIO_DEV_MAX);
		fclose(fp);
	}
	sink_devs_ms = src_devs_ms = monotonic_msec();
	audio_popup_data_arrived();
}

static void
fetch_audio_devices(int sources)
{
	uint64_t now = monotonic_msec();
	uint64_t *ms = sources ? &src_devs_ms : &sink_devs_ms;

	if (*ms && now >= *ms && now - *ms < 2000)
		return;
	if (devs_fetch_inflight)
		return;
	if (fetch_async("wpctl status", devs_fetch_done, NULL) == 0)
		devs_fetch_inflight = 1;
}

/* ── gauge slider drag state ─────────────────────────────────────── */

static struct {
	int active;        /* 0 none, 1 volume, 2 mic, 3 brightness,
	                    * 4 fan gauge, 5 fan curve point */
	Monitor *mon;
	double frac;
	uint64_t last_set_ms;
} sdrag;

/* Fan popup edit state: which fan row is expanded and the curve being
 * edited (a local copy — the published snapshot must not overwrite the
 * table mid-drag).  Committed to fanwatch + fans.conf on release. */
static struct {
	int flat;          /* expanded flat fan index, -1 none */
	FanCurve curve;
	int sel;           /* selected curve point, -1 */
} fedit = { -1, {{0},{0},0}, -1 };

static InfoPopup *
sdrag_popup(void)
{
	if (!sdrag.active || !sdrag.mon)
		return NULL;
	if (sdrag.active >= 4)
		return &sdrag.mon->statusbar.fan_popup;
	return sdrag.active == 1 ? &sdrag.mon->statusbar.volume_popup :
		sdrag.active == 2 ? &sdrag.mon->statusbar.mic_popup :
		&sdrag.mon->statusbar.light_popup;
}

/* fan_pub entry + owning device for a flat index (compositor thread). */
static FanEntry *
fan_pub_flat(int idx, FanDevice **devout)
{
	int n = 0;

	for (int d = 0; d < fan_pub.ndevices; d++)
		for (int f = 0; f < fan_pub.devices[d].fan_count; f++)
			if (n++ == idx) {
				if (devout)
					*devout = &fan_pub.devices[d];
				return &fan_pub.devices[d].fans[f];
			}
	return NULL;
}

/* Persist one fan's mode/level/curve to fans.conf. */
static void
fan_store_conf(int flat, int mode, int manual_pct, const FanCurve *c)
{
	FanDevice *dev;
	FanEntry *fe = fan_pub_flat(flat, &dev);
	char key[96];

	if (!fe)
		return;
	fan_conf_key(dev, fe, key, sizeof(key));
	fanconf_store(key, mode, manual_pct >= 0 ? manual_pct : fe->manual_pct,
			c ? c : &fe->curve);
}

/* Push the dragged fraction to PipeWire + bar module. */
static void
slider_commit(void)
{
	double pct = sdrag.frac * 100.0;
	uint64_t now = monotonic_msec();

	if (sdrag.active == 1) {
		int is_headset = pipewire_sink_is_headset_nb();

		set_pipewire_volume(pct);
		if (pct <= 0.0) {
			/* dragged to zero = mute */
			set_pipewire_mute(1);
			volume_cache_store(is_headset, 0.0, 1, now);
		} else {
			if (volume_muted == 1)
				set_pipewire_mute(0);
			volume_cache_store(is_headset, pct, 0, now);
		}
		speaker_active = pct;
		refreshstatusvolume();
	} else if (sdrag.active == 2) {
		set_pipewire_mic_volume(pct);
		if (pct <= 0.0) {
			/* dragged to zero = mute */
			set_pipewire_mic_mute(1);
		} else if (mic_muted == 1) {
			set_pipewire_mic_mute(0);
		}
		mic_last_percent = pct;
		mic_cached = pct;
		mic_last_read_ms = now;
		microphone_active = pct;
		refreshstatusmic();
	} else if (sdrag.active == 3) {
		set_backlight_percent(pct);
		light_mode_set_manual(pct);
		refreshstatuslight();
	} else if (sdrag.active == 4) {
		/* queued to fanwatch's thread — sysfs/EC writes block */
		fanwatch_set_frac(fedit.flat, sdrag.frac);
	}
}

/* Meter heartbeat at draw rate: accumulate peaks every tick, push a
 * history sample every METER_PUSH_MS, and redraw each tick with the
 * sub-pixel slide so the scroll is smooth. Stops itself (and the
 * pw-record child) the moment no audio popup is visible. */

/* Capturing from a bluez mic makes WirePlumber flip the headset
 * A2DP→HFP: the A2DP sink vanishes mid-playback and players pause.
 * The live meter is not worth that — BT default mics (and an unknown
 * device list) get a flat meter instead of a capture stream. */
static int
mic_meter_blocked(void)
{
	if (src_dev_count == 0)
		return 1;
	for (int i = 0; i < src_dev_count; i++)
		if (src_devs[i].is_default)
			return src_devs[i].is_headset;
	return 0;
}

static int
meter_tick(void *data)
{
	Monitor *m;
	InfoPopup *p = NULL;
	MeterUI *mu;
	int is_mic = 0;
	double peak;
	uint64_t now = monotonic_msec();

	(void)data;
	wl_list_for_each(m, &mons, link) {
		if (m->statusbar.volume_popup.visible) {
			p = &m->statusbar.volume_popup;
			is_mic = 0;
			break;
		}
		if (m->statusbar.mic_popup.visible) {
			p = &m->statusbar.mic_popup;
			is_mic = 1;
			break;
		}
	}
	if (!p) {
		audio_meter_stop();
		memset(meter_ui[0].hist, 0, sizeof(meter_ui[0].hist));
		memset(meter_ui[1].hist, 0, sizeof(meter_ui[1].hist));
		meter_ui[0].node = meter_ui[1].node = NULL;
		card_meter_raster_finish(&meter_ui[0].raster);
		card_meter_raster_finish(&meter_ui[1].raster);
		meter_pend = 0.0f;
		meter_push_ms = 0;
		return 0;   /* stays disarmed until the next popup render */
	}

	if (is_mic && mic_meter_blocked())
		audio_meter_stop();
	else
		audio_meter_start(is_mic);
	peak = audio_meter_take_peak();
	if ((is_mic ? mic_muted : volume_muted) == 1)
		peak = 0.0;
	if (peak > 1.0)
		peak = 1.0;
	if ((float)peak > meter_pend)
		meter_pend = (float)peak;

	mu = &meter_ui[is_mic];
	if (!meter_push_ms || now - meter_push_ms >= METER_PUSH_MS) {
		mu->head = (mu->head + 1) % METER_HIST;
		mu->hist[mu->head] = meter_pend;
		meter_pend = 0.0f;
		/* keep the cadence stable; resync after a long stall */
		if (meter_push_ms && now - meter_push_ms < 4 * METER_PUSH_MS)
			meter_push_ms += METER_PUSH_MS;
		else
			meter_push_ms = now;
	}

	/* Drawing happens in meter_frame_tick (one exact-phase redraw per
	 * displayed frame); here just keep the vblank chain alive in case
	 * the self-sustaining damage loop ever breaks. */
	if (m->wlr_output)
		wlr_output_schedule_frame(m->wlr_output);
	meter_timer_arm();
	return 0;
}

/* Called from rendermon once per output frame while an audio popup is
 * visible: redraw the meter with the phase at draw time, so the scroll
 * is locked to the display clock instead of beating against the 16 ms
 * timer (the source of the occasional judder).  Each redraw damages the
 * scene, which schedules the next frame — a self-sustaining per-frame
 * loop that stops as soon as the popup hides. */
void
meter_frame_tick(Monitor *m)
{
	static uint64_t last_raster_ms;
	InfoPopup *p;
	int is_mic;
	uint64_t now;

	if (m->statusbar.volume_popup.visible) {
		p = &m->statusbar.volume_popup;
		is_mic = 0;
	} else if (m->statusbar.mic_popup.visible) {
		p = &m->statusbar.mic_popup;
		is_mic = 1;
	} else {
		return;
	}
	/* ~60 Hz cap: the bars are 3 px wide — re-rastering per vblank on
	 * a 144 Hz panel is invisible.  The 16 ms meter timer keeps the
	 * vblank chain alive, so skipped ticks cost nothing. */
	now = monotonic_msec();
	if (now - last_raster_ms < 16)
		return;
	last_raster_ms = now;
	meter_overlay_draw(p, is_mic, meter_phase(now));
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
	card_at(m, m->statusbar.area.x + p->tree->node.x,
			m->statusbar.area.y + statusbar_popup_y(m));

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

/* Connection-type icon for a sink/source, keyed on the wpctl name:
 * HDMI/DP/USB/jack/bluetooth, with a speaker/mic fallback. */
static const char *
audio_dev_svg(const AudioDevice *d, int is_mic)
{
	char low[64];
	int i;

	for (i = 0; d->name[i] && i < (int)sizeof(low) - 1; i++)
		low[i] = (char)tolower((unsigned char)d->name[i]);
	low[i] = '\0';
	if (d->is_headset)
		return "images/svg/bt_headset.svg";
	if (strstr(low, "hdmi"))
		return "images/svg/audio_hdmi.svg";
	if (strstr(low, "displayport") || strstr(low, "display port"))
		return "images/svg/audio_dp.svg";
	if (strstr(low, "usb"))
		return "images/svg/audio_usb.svg";
	if (strstr(low, "headset") || strstr(low, "headphone") ||
			strstr(low, "buds") || strstr(low, "pods") ||
			strstr(low, "arctis"))
		return "images/svg/bt_headset.svg";
	if (strstr(low, "analog") || strstr(low, "jack") ||
			strstr(low, "line"))
		return "images/svg/audio_jack.svg";
	return is_mic ? "images/svg/audio_mic.svg" :
		"images/svg/bt_speaker.svg";
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

	/* non-blocking: cached state now, async refresh re-renders when the
	 * fresh answer lands (audio_popup_data_arrived) */
	if (sdrag.active && sdrag_popup() == p)
		vol = sdrag.frac * 100.0;
	else if (is_mic)
		vol = pipewire_mic_volume_percent_nb();
	else
		vol = pipewire_volume_percent_nb(&is_headset);

	card = card_begin();
	if (!card)
		return;
	card_at(m, m->statusbar.area.x + p->tree->node.x,
			m->statusbar.area.y + statusbar_popup_y(m));

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
	card_gap(card, 4);
	card_meter(card);
	card_gap(card, 4);

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
		card_section(card, is_mic ? "INPUT DEVICES" : "OUTPUT DEVICES");
		for (int i = 0; i < dev_count; i++) {
			AudioDevice *d = &devs[i];
			char name[36];

			snprintf(name, sizeof(name), "%.33s", d->name);
			/* BT-popup style: whole row is the click target with a
			 * hover wash; the default device reads "Active" */
			card_icon_text_hit(card, audio_dev_svg(d, is_mic),
					name, d->is_default ? "Active" : NULL,
					card_col_green, NULL,
					d->is_default ? -1 : i,
					p->btn_hover == i);
		}
	}

	if (card_finish(card, &res) != 0)
		return;
	memcpy(p->hits, res.hits, sizeof(p->hits));
	p->nhits = res.nhits;
	popup_view_apply(&p->view, p->tree, &res);
	p->width = p->view.w;
	p->height = p->view.h;

	/* apply destroyed the previous overlay with the old card content;
	 * redraw right away so the meter never blinks out for a frame */
	meter_ui[is_mic].node = NULL;
	meter_ui[is_mic].x = res.meter_x;
	meter_ui[is_mic].y = res.meter_y;
	meter_ui[is_mic].w = res.meter_w;
	meter_ui[is_mic].h = res.meter_h;
	meter_overlay_draw(p, is_mic, meter_phase(monotonic_msec()));
	meter_timer_arm();
}

static void
render_volume_popup(Monitor *m)
{
	render_audio_popup(m, &m->statusbar.volume_popup, 0);
}

/* Async wpctl state landed (volume/mic level, headset probe, device
 * list): re-render the audio popup that is on screen so the fresh data
 * shows without waiting for the next cursor motion. */
void
audio_popup_data_arrived(void)
{
	Monitor *m;

	wl_list_for_each(m, &mons, link) {
		if (m->statusbar.volume_popup.visible) {
			render_audio_popup(m, &m->statusbar.volume_popup, 0);
			m->statusbar.volume_popup.last_render_ms =
				monotonic_msec();
			return;
		}
		if (m->statusbar.mic_popup.visible) {
			render_audio_popup(m, &m->statusbar.mic_popup, 1);
			m->statusbar.mic_popup.last_render_ms =
				monotonic_msec();
			return;
		}
	}
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
	card_at(m, m->statusbar.area.x + p->tree->node.x,
			m->statusbar.area.y + statusbar_popup_y(m));

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
	card_kv2(card, "Device", dev[0] ? dev : "--", NULL, "Ambient",
			light_ambient_luma < 0 ? "--" :
			light_ambient_luma <= 25 ? "Dark" :
			light_ambient_luma <= 85 ? "Dim" :
			light_ambient_luma <= 150 ? "Normal" : "Bright", NULL);

	{
		static const char *labels[2] = { "Auto", "Manual" };

		card_section(card, "MODE");
		card_buttons(card, labels, NULL, 2, light_auto_mode ? 0 : 1,
				p->btn_hover, LIGHT_MODE_HIT_BASE);
	}

	if (card_finish(card, &res) != 0)
		return;
	memcpy(p->hits, res.hits, sizeof(p->hits));
	p->nhits = res.nhits;
	popup_view_apply(&p->view, p->tree, &res);
	p->width = p->view.w;
	p->height = p->view.h;
}

static int info_popup_clamped_x(Monitor *m, StatusModule *mod, InfoPopup *p);
static CardHit *popup_hit_by_id(InfoPopup *p, int id);

/* Fan overview + control, grouped CPU / GPU / OTHER.  Every fan row
 * shows live speed + temperature; controllable fans expand (Adjust) to
 * an Auto / Manual / Curve mode switch with a draggable speed gauge and
 * an editable temp→speed curve.  Covers hwmon pwm (desktop and most
 * laptop EC drivers), the MSI EC tables via nixly-fand, and desktop
 * NVIDIA via NVML.  Replaces mcontrolcenter. */
static void
render_fan_popup(Monitor *m)
{
	static const char *sect_label[3] =
		{ "CPU FANS", "GPU FANS", "OTHER FANS" };
	InfoPopup *p = &m->statusbar.fan_popup;
	Card *card;
	CardResult res;
	char value[24], v1[48];

	if (!p->tree || fan_pub.total_fans <= 0)
		return;
	/* Renders from the last published snapshot — sampling the msi-ec
	 * here would stall the cursor.  Just ask fanwatch to sample faster
	 * while the card is up; the request lapses once it closes. */
	fanwatch_poke_fast();

	card = card_begin();
	if (!card)
		return;
	card_at(m, m->statusbar.area.x + p->tree->node.x,
			m->statusbar.area.y + statusbar_popup_y(m));

	if (fan_primary_value(value, sizeof(value)) != 0)
		snprintf(value, sizeof(value), "--");
	card_header(card, fan_icon_path, "Fans", "COOLING", value);
	card_gap(card, 6);

	for (int sec = 0; sec < 3; sec++) {
		int flat = -1, shown = 0;

		for (int d = 0; d < fan_pub.ndevices; d++) {
			FanDevice *dev = &fan_pub.devices[d];

			for (int f = 0; f < dev->fan_count; f++) {
				FanEntry *fe = &dev->fans[f];
				const float *vcol;
				int expanded;

				flat++;
				if (fan_entry_section(dev, fe) != sec)
					continue;
				if (!shown) {
					card_section(card, sect_label[sec]);
					shown = 1;
				}
				expanded = fedit.flat == flat;

				if (fan_shows_pct(fe))
					snprintf(v1, sizeof(v1), "%d%%",
							fe->rpm);
				else
					snprintf(v1, sizeof(v1), "%d RPM",
							fe->rpm);
				if (fe->temp_mc > 0) {
					size_t n = strlen(v1);

					snprintf(v1 + n, sizeof(v1) - n,
							" \302\267 %d\302\260C",
							fe->temp_mc / 1000);
				}
				vcol = fe->ctl == FAN_CTL_NONE ? card_col_dim :
					fe->mode == FAN_MODE_MANUAL ?
						card_col_yellow :
					fe->mode == FAN_MODE_CURVE ?
						card_col_blue : card_col_green;
				card_text_btn(card, fe->label, v1, vcol,
						fe->ctl != FAN_CTL_NONE ?
						(expanded ? "Close" : "Adjust")
						: NULL,
						fe->ctl != FAN_CTL_NONE ?
						FAN_ROW_BASE + flat : -1,
						p->btn_hover ==
						FAN_ROW_BASE + flat);

				if (!expanded || fe->ctl == FAN_CTL_NONE)
					continue;
				{
					static const char *modes[3] =
						{ "Auto", "Manual", "Curve" };
					int hov = p->btn_hover >= FAN_MODE_HIT &&
						p->btn_hover < FAN_MODE_HIT + 3 ?
						p->btn_hover - FAN_MODE_HIT : -1;

					card_gap(card, 4);
					card_buttons(card, modes, NULL, 3,
							fe->mode, hov,
							FAN_MODE_HIT);
				}
				if (fe->mode == FAN_MODE_MANUAL) {
					double frac = sdrag.active == 4 ?
						sdrag.frac :
						fe->manual_pct / 100.0;

					card_gap(card, 6);
					card_gauge_id(card, frac,
							card_col_yellow,
							FAN_SLIDER_HIT);
					card_gap(card, 2);
				}
				if (fe->mode == FAN_MODE_CURVE) {
					card_gap(card, 6);
					card_curve(card, fedit.curve.temp,
							fedit.curve.pct,
							FAN_CURVE_PTS,
							fedit.sel,
							card_col_blue,
							FAN_CURVE_HIT);
					card_gap(card, 2);
				}
			}
		}
	}

	if (fan_pub.has_msi && !fan_pub.helper_ok) {
		card_section(card, NULL);
		card_text(card, "Curve control needs nixly-fand",
				"helper off", card_col_dim);
	}

	if (fan_pub.has_msi) {
		card_section(card, "COOLER BOOST");
		card_kv2_btn(card, "All fans", "Max speed", NULL, "State",
				fan_pub.cooler_boost_on ? "On" : "Off",
				fan_pub.cooler_boost_on ? card_col_red : NULL,
				FAN_BOOST_HIT, p->btn_hover == FAN_BOOST_HIT);
	}

	if (card_finish(card, &res) != 0)
		return;
	memcpy(p->hits, res.hits, sizeof(p->hits));
	p->nhits = res.nhits;
	popup_view_apply(&p->view, p->tree, &res);
	p->width = p->view.w;
	p->height = p->view.h;
}

/* Click in the fan popup: pwm gauge = set that fan's speed (manual),
 * "Auto" = back to the firmware curve, boost chip = toggle. */
int
fan_popup_handle_click(Monitor *m, int lx, int ly, uint32_t button)
{
	InfoPopup *p = &m->statusbar.fan_popup;
	int popup_x, rel_x, rel_y;

	if (!p->visible || button != BTN_LEFT)
		return 0;

	popup_x = info_popup_clamped_x(m, &m->statusbar.fan, p);
	rel_x = lx - popup_x;
	rel_y = ly - statusbar_popup_y(m);
	if (rel_x < 0 || rel_y < 0 || rel_x >= p->width || rel_y >= p->height)
		return 0;

	/* The sysfs/EC writes are queued to fanwatch's thread, not done
	 * here — the msi-ec ones block as long as its reads do.  The card
	 * redraws from the snapshot the worker publishes after applying. */
	for (int i = 0; i < p->nhits; i++) {
		CardHit *hit = &p->hits[i];
		FanEntry *fe;

		if (hit->w <= 0 ||
				rel_x < hit->x || rel_x >= hit->x + hit->w ||
				rel_y < hit->y || rel_y >= hit->y + hit->h)
			continue;
		if (hit->id >= FAN_ROW_BASE &&
				hit->id < FAN_ROW_BASE + FAN_MAX_TOTAL) {
			int flat = hit->id - FAN_ROW_BASE;

			fe = fan_pub_flat(flat, NULL);
			if (fe && fedit.flat != flat) {
				fedit.flat = flat;
				fedit.curve = fe->curve;
				fedit.sel = -1;
			} else {
				fedit.flat = -1;
			}
			p->last_render_ms = 0;
			return 1;
		}
		if (hit->id >= FAN_MODE_HIT && hit->id < FAN_MODE_HIT + 3) {
			int mode = hit->id - FAN_MODE_HIT;

			fe = fan_pub_flat(fedit.flat, NULL);
			if (!fe || fe->ctl == FAN_CTL_NONE)
				return 1;
			switch (mode) {
			case FAN_MODE_AUTO:
				fanwatch_set_auto(fedit.flat);
				break;
			case FAN_MODE_MANUAL:
				fanwatch_set_frac(fedit.flat,
						fe->manual_pct / 100.0);
				break;
			case FAN_MODE_CURVE:
				fanwatch_set_curve(fedit.flat, &fedit.curve);
				break;
			}
			fan_store_conf(fedit.flat, mode, -1, &fedit.curve);
			p->last_render_ms = 0;
			return 1;
		}
		if (hit->id == FAN_SLIDER_HIT) {
			double frac = (double)(rel_x - hit->x) / hit->w;

			if (frac < 0.0)
				frac = 0.0;
			if (frac > 1.0)
				frac = 1.0;
			sdrag.active = 4;
			sdrag.mon = m;
			sdrag.frac = frac;
			sdrag.last_set_ms = monotonic_msec();
			popup_view_drag_fill_frac(&p->view, 0, frac);
			slider_commit();
			return 1;
		}
		if (hit->id == FAN_CURVE_HIT) {
			/* pick the nearest point, then drag it */
			double fx = (double)(rel_x - hit->x) / hit->w;
			double fy = (double)(rel_y - hit->y) / hit->h;
			double t = CARD_CURVE_TMIN +
				fx * (CARD_CURVE_TMAX - CARD_CURVE_TMIN);
			int best = 0;
			double bestd = 1e9;

			for (int j = 0; j < FAN_CURVE_PTS; j++) {
				double dt = (t - fedit.curve.temp[j]) /
					(CARD_CURVE_TMAX - CARD_CURVE_TMIN);
				double dp = (1.0 - fy) -
					fedit.curve.pct[j] / 100.0;
				double dd = dt * dt + dp * dp;

				if (dd < bestd) {
					bestd = dd;
					best = j;
				}
			}
			fedit.sel = best;
			sdrag.active = 5;
			sdrag.mon = m;
			sdrag.last_set_ms = 0;
			p->last_render_ms = 0;
			return 1;
		}
		if (hit->id == FAN_BOOST_HIT) {
			fanwatch_set_boost(!fan_pub.cooler_boost_on);
			p->last_render_ms = 0;
			return 1;
		}
	}
	/* swallow clicks on the card body */
	return 1;
}

/* Ongoing curve-point drag: move the selected point with the cursor
 * (temp clamped between its neighbours, speed 0-100 in 5% steps) and
 * re-render the card at ~25 fps.  The table is committed to fanwatch +
 * fans.conf on release. */
static void
fan_curve_drag_motion(Monitor *m, double cx, double cy)
{
	InfoPopup *p = &m->statusbar.fan_popup;
	CardHit *hit;
	int popup_x, rel_x, rel_y, t, pct, lo, hi;
	uint64_t now;

	if (sdrag.active != 5 || sdrag.mon != m || !p->visible ||
			fedit.sel < 0)
		return;
	hit = popup_hit_by_id(p, FAN_CURVE_HIT);
	if (!hit)
		return;
	popup_x = info_popup_clamped_x(m, &m->statusbar.fan, p);
	rel_x = (int)floor(cx) - m->statusbar.area.x - popup_x;
	rel_y = (int)floor(cy) - m->statusbar.area.y - statusbar_popup_y(m);

	t = CARD_CURVE_TMIN + (int)lround(
			(double)(rel_x - hit->x) / hit->w *
			(CARD_CURVE_TMAX - CARD_CURVE_TMIN));
	pct = (int)lround((1.0 - (double)(rel_y - hit->y) / hit->h) * 20.0)
			* 5;
	lo = fedit.sel > 0 ? fedit.curve.temp[fedit.sel - 1] + 1 :
			CARD_CURVE_TMIN;
	hi = fedit.sel < FAN_CURVE_PTS - 1 ?
			fedit.curve.temp[fedit.sel + 1] - 1 : CARD_CURVE_TMAX;
	if (t < lo)
		t = lo;
	if (t > hi)
		t = hi;
	if (pct < 0)
		pct = 0;
	if (pct > 100)
		pct = 100;
	if (fedit.curve.temp[fedit.sel] == t &&
			fedit.curve.pct[fedit.sel] == pct)
		return;
	fedit.curve.temp[fedit.sel] = (uint8_t)t;
	fedit.curve.pct[fedit.sel] = (uint8_t)pct;
	if (fedit.sel == 0)
		fedit.curve.base = (uint8_t)pct; /* floor follows point 0 */

	now = monotonic_msec();
	if (now - sdrag.last_set_ms >= 40) {
		sdrag.last_set_ms = now;
		render_fan_popup(m);
		p->last_render_ms = now;
	}
}

/* ── generic hover plumbing ──────────────────────────────────────── */

/* Re-render cadence while visible; volume/light react to scroll wheel
 * changes on the module underneath the cursor. */
#define INFO_RERENDER_MS 500
/* Cursor must stay outside icon+popup this long before an info popup
 * hides — crossing the bar→popup gap never closes it. */
#define INFO_POPUP_GRACE_MS 250

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

	if (p->visible) {
		/* Anchor stays where the popup opened: the module's x shifts
		 * as its text width changes (70% -> 5%) and must not yank
		 * the popup — and its hover rect — out from under the
		 * cursor mid-interaction. */
		popup_x = p->tree->node.x;
	} else {
		popup_x = mod->x;
	}
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
			ly < statusbar_popup_y(m) + p->height)
		inside = 1;

	/* an active gauge drag holds its popup open even when the cursor
	 * leaves the card; same for a display-box reorder drag */
	if (sdrag_popup() == p || display_drag_popup() == p)
		inside = 1;

	was_visible = p->visible;

	/* hover highlight for device "Use" buttons */
	if (inside && p->visible && p->nhits > 0) {
		int rel_x = lx - popup_x;
		int rel_y = ly - statusbar_popup_y(m);
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
		p->outside_since_ms = 0;
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
			/* re-clamp with the fresh size (fresh anchor only on
			 * show — see the stable-anchor comment above) */
			if (!was_visible)
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
				popup_x, statusbar_popup_y(m));
		if (!was_visible)
			popup_view_show(&p->view);
	} else if (p->visible) {
		/* Close-grace: a transit through the bar→popup gap strip or
		 * a brief overshoot outside the card must not kill the
		 * popup.  Only hide once the cursor has stayed outside
		 * continuously for the whole grace window. */
		if (p->outside_since_ms == 0) {
			p->outside_since_ms = now;
			schedule_popup_delay(INFO_POPUP_GRACE_MS + 1);
			return;
		}
		if (now - p->outside_since_ms < INFO_POPUP_GRACE_MS) {
			schedule_popup_delay(INFO_POPUP_GRACE_MS -
					(now - p->outside_since_ms) + 1);
			return;
		}
		p->visible = 0;
		p->outside_since_ms = 0;
		p->hover_start_ms = 0;
		p->last_render_ms = 0;
		p->btn_hover = -1;
		popup_view_hide(&p->view);
		wlr_scene_node_set_enabled(&p->tree->node, 0);
	} else if (p->hover_start_ms != 0) {
		p->hover_start_ms = 0;
	}
}

/* Clamp like info_popup_hover does so click/drag coordinates line up
 * with where the popup is actually drawn. */
static int
info_popup_clamped_x(Monitor *m, StatusModule *mod, InfoPopup *p)
{
	/* While visible, use the popup's actual position (the anchor is
	 * frozen at show time — see info_popup_hover) so click and drag
	 * coordinates always match what is on screen. */
	int popup_x = p->visible && p->tree ? p->tree->node.x : mod->x;

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
popup_hit_by_id(InfoPopup *p, int id)
{
	for (int i = 0; i < p->nhits; i++)
		if (p->hits[i].id == id && p->hits[i].w > 0)
			return &p->hits[i];
	return NULL;
}

static CardHit *
info_popup_slider_hit(InfoPopup *p)
{
	return popup_hit_by_id(p, SLIDER_HIT_ID);
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

	if (!p || sdrag.mon != m || sdrag.active == 5)
		return;
	mod = sdrag.active == 1 ? &m->statusbar.volume :
		sdrag.active == 2 ? &m->statusbar.mic :
		sdrag.active == 3 ? &m->statusbar.light : &m->statusbar.fan;
	track = sdrag.active == 4 ? popup_hit_by_id(p, FAN_SLIDER_HIT) :
		info_popup_slider_hit(p);
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
	popup_view_drag_fill_frac(&p->view, 0, frac);

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
	rel_y = ly - statusbar_popup_y(m);
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
			popup_view_drag_fill_frac(&p->view, 0, frac);
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
			/* manual pick beats the headset-sink guard */
			if (!is_mic)
				audio_headset_sink_override(
						!devs[hit->id].is_headset);
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
	rel_y = ly - statusbar_popup_y(m);
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
		popup_view_drag_fill_frac(&p->view, 0, frac);
		slider_commit();
		return 1;
	}

	/* Auto/Manual mode buttons */
	for (int i = 0; i < p->nhits; i++) {
		CardHit *h = &p->hits[i];

		if (h->id < LIGHT_MODE_HIT_BASE || h->id > LIGHT_MODE_HIT_BASE + 1)
			continue;
		if (rel_x < h->x || rel_x >= h->x + h->w ||
				rel_y < h->y || rel_y >= h->y + h->h)
			continue;
		if (h->id == LIGHT_MODE_HIT_BASE) {
			light_mode_set_auto();
		} else {
			/* restore the remembered manual level; none stored →
			 * lock the current one */
			double v = light_manual_value >= 0.0 ?
				light_manual_value : backlight_percent();

			light_mode_set_manual(v);
			if (v >= 0.0 && set_backlight_percent(v) == 0) {
				light_last_percent = v;
				light_cached_percent = v;
				refreshstatuslight();
			}
		}
		p->last_render_ms = 0;
		return 1;
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
	if (sdrag.active == 5) {
		/* curve drag: commit the edited table + persist */
		fanwatch_set_curve(fedit.flat, &fedit.curve);
		fan_store_conf(fedit.flat, FAN_MODE_CURVE, -1, &fedit.curve);
	} else {
		slider_commit();
		if (sdrag.active == 4)
			fan_store_conf(fedit.flat, FAN_MODE_MANUAL,
					(int)lround(sdrag.frac * 100.0), NULL);
	}
	p->last_render_ms = 0;   /* refresh the % text with the final level */
	sdrag.active = 0;
	sdrag.mon = NULL;
}

/* Volume/mic/light just changed (drag, scroll, keybind, external): mark
 * any open popup stale and schedule a hover re-poll so the popup's %
 * and gauge follow the bar module in lock-step. */
void
info_popup_mark_stale(void)
{
	Monitor *m;
	int any = 0;

	wl_list_for_each(m, &mons, link) {
		if (m->statusbar.volume_popup.visible) {
			m->statusbar.volume_popup.last_render_ms = 0;
			any = 1;
		}
		if (m->statusbar.mic_popup.visible) {
			m->statusbar.mic_popup.last_render_ms = 0;
			any = 1;
		}
		if (m->statusbar.light_popup.visible) {
			m->statusbar.light_popup.last_render_ms = 0;
			any = 1;
		}
	}
	if (any)
		schedule_popup_delay(1);
}

void
updateinfopopups(Monitor *m, double cx, double cy)
{
	if (!m)
		return;
	if (sdrag.active)
		slider_drag_motion(m, cx);
	fan_curve_drag_motion(m, cx, cy);
	display_drag_motion(m, cx);
	info_popup_hover(m, &m->statusbar.clock, &m->statusbar.clock_popup,
			render_clock_popup, cx, cy);
	info_popup_hover(m, &m->statusbar.volume, &m->statusbar.volume_popup,
			render_volume_popup, cx, cy);
	info_popup_hover(m, &m->statusbar.mic, &m->statusbar.mic_popup,
			render_mic_popup, cx, cy);
	info_popup_hover(m, &m->statusbar.light, &m->statusbar.light_popup,
			render_light_popup, cx, cy);
	info_popup_hover(m, &m->statusbar.fan, &m->statusbar.fan_popup,
			render_fan_popup, cx, cy);
	info_popup_hover(m, &m->statusbar.bluetooth, &m->statusbar.bt_popup,
			render_bt_popup, cx, cy);
	info_popup_hover(m, &m->statusbar.display, &m->statusbar.display_popup,
			render_display_popup, cx, cy);
	info_popup_hover(m, &m->statusbar.power, &m->statusbar.power_popup,
			render_power_popup, cx, cy);
}

/* 1 while some info popup is waiting out its show delay — the shared
 * popup-delay timer uses this to know it must re-poll. */
int
info_popup_pending(Monitor *m)
{
	return m->statusbar.clock_popup.outside_since_ms != 0 ||
		m->statusbar.volume_popup.outside_since_ms != 0 ||
		m->statusbar.mic_popup.outside_since_ms != 0 ||
		m->statusbar.light_popup.outside_since_ms != 0 ||
		m->statusbar.fan_popup.outside_since_ms != 0 ||
		m->statusbar.bt_popup.outside_since_ms != 0 ||
		m->statusbar.display_popup.outside_since_ms != 0 ||
		m->statusbar.power_popup.outside_since_ms != 0 ||
		(m->statusbar.power_popup.hover_start_ms != 0 &&
			!m->statusbar.power_popup.visible) ||
		(m->statusbar.clock_popup.hover_start_ms != 0 &&
			!m->statusbar.clock_popup.visible) ||
		(m->statusbar.volume_popup.hover_start_ms != 0 &&
			!m->statusbar.volume_popup.visible) ||
		(m->statusbar.mic_popup.hover_start_ms != 0 &&
			!m->statusbar.mic_popup.visible) ||
		(m->statusbar.light_popup.hover_start_ms != 0 &&
			!m->statusbar.light_popup.visible) ||
		(m->statusbar.fan_popup.hover_start_ms != 0 &&
			!m->statusbar.fan_popup.visible) ||
		(m->statusbar.bt_popup.hover_start_ms != 0 &&
			!m->statusbar.bt_popup.visible) ||
		(m->statusbar.display_popup.hover_start_ms != 0 &&
			!m->statusbar.display_popup.visible);
}

int
info_popup_visible(Monitor *m)
{
	return m->statusbar.clock_popup.visible ||
		m->statusbar.volume_popup.visible ||
		m->statusbar.mic_popup.visible ||
		m->statusbar.light_popup.visible ||
		m->statusbar.fan_popup.visible ||
		m->statusbar.bt_popup.visible ||
		m->statusbar.display_popup.visible ||
		m->statusbar.power_popup.visible;
}

void
info_popups_hide(Monitor *m)
{
	InfoPopup *ps[8] = { &m->statusbar.clock_popup,
		&m->statusbar.volume_popup, &m->statusbar.mic_popup,
		&m->statusbar.light_popup, &m->statusbar.fan_popup,
		&m->statusbar.bt_popup, &m->statusbar.display_popup,
		&m->statusbar.power_popup };

	for (int i = 0; i < 8; i++) {
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
