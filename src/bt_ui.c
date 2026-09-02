/* Bluetooth statusbar module + popup (replaces blueman).  Pure view on
 * btmon's model: power toggle, discovery, pair/connect/forget, device
 * battery.  Left-click a device = pair or connect/disconnect;
 * right-click = forget.
 */
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "nixlytile.h"
#include "netsys.h"
#include "popup_card.h"

#define BT_HIT_POWER 210
#define BT_HIT_SCAN  211
#define BT_HIT_DEV   220   /* + device index */

char bt_icon_path[PATH_MAX] = "images/svg/bluetooth_searching.svg";
char bt_icon_loaded_path[PATH_MAX];
int bt_icon_loaded_h, bt_icon_w, bt_icon_h;
struct wlr_buffer *bt_icon_buf;
static char bt_text[64];
static BtDev bt_ui_devs[BT_DEV_MAX];
static int bt_ui_ndevs;

void
btsys_changed(void)
{
	trigger_status_task_now(refreshstatusbluetooth);
}

/* Device-type icon: bluez Icon hint first, name keywords as fallback. */
static const char *
bt_dev_svg(const BtDev *d)
{
	char low[64];
	int i;

	for (i = 0; d->name[i] && i < (int)sizeof(low) - 1; i++)
		low[i] = (char)tolower((unsigned char)d->name[i]);
	low[i] = '\0';
	/* TVs advertise as plain audio sinks — name beats icon there */
	if (strstr(low, "[tv]") || strstr(low, "bravia"))
		return "images/svg/bt_display.svg";
	if (strstr(d->icon, "gaming"))
		return "images/svg/bt_controller.svg";
	if (strstr(d->icon, "headset") || strstr(d->icon, "headphone"))
		return "images/svg/bt_headset.svg";
	if (strstr(d->icon, "keyboard"))
		return "images/svg/bt_keyboard.svg";
	if (strstr(d->icon, "mouse") || strstr(d->icon, "tablet"))
		return "images/svg/bt_mouse.svg";
	if (strstr(d->icon, "display") || strstr(d->icon, "video"))
		return "images/svg/bt_display.svg";
	if (strncmp(d->icon, "phone", 5) == 0)
		return "images/svg/bt_phone.svg";
	if (strstr(d->icon, "computer"))
		return "images/svg/bt_computer.svg";
	if (strstr(d->icon, "watch"))
		return "images/svg/bt_watch.svg";
	if (strstr(d->icon, "audio-card") || strstr(d->icon, "multimedia"))
		return "images/svg/bt_speaker.svg";

	for (i = 0; d->name[i] && i < (int)sizeof(low) - 1; i++)
		low[i] = (char)tolower((unsigned char)d->name[i]);
	low[i] = '\0';
	if (strstr(low, "controller") || strstr(low, "gamepad") ||
			strstr(low, "dualsense") || strstr(low, "dualshock") ||
			strstr(low, "xbox") || strstr(low, "joy-con"))
		return "images/svg/bt_controller.svg";
	if (strstr(low, "headset") || strstr(low, "headphone") ||
			strstr(low, "buds") || strstr(low, "pods") ||
			strstr(low, "arctis"))
		return "images/svg/bt_headset.svg";
	if (strstr(low, "keyboard"))
		return "images/svg/bt_keyboard.svg";
	if (strstr(low, "mouse"))
		return "images/svg/bt_mouse.svg";
	if (strstr(low, "speaker") || strstr(low, "soundbar"))
		return "images/svg/bt_speaker.svg";
	if (strstr(low, " tv") || strncmp(low, "tv ", 3) == 0 ||
			strstr(low, "bravia") || strstr(low, "shield"))
		return "images/svg/bt_display.svg";
	if (strstr(low, "watch"))
		return "images/svg/bt_watch.svg";
	return "images/svg/bluetooth.svg";
}

/* Link-quality bars for a connected device. BR/EDR RSSI 0 means "in
 * golden range" but doubles as the no-reading sentinel, so callers only
 * ask for an icon when rssi != 0. */
static const char *
bt_sig_svg(int rssi)
{
	if (rssi >= -60)
		return "images/svg/wifi_100.svg";
	if (rssi >= -70)
		return "images/svg/wifi_75.svg";
	if (rssi >= -80)
		return "images/svg/wifi_50.svg";
	return "images/svg/wifi_25.svg";
}

static const char *
bt_batt_svg(int pct)
{
	if (pct > 75)
		return "images/svg/bt_batt_100.svg";
	if (pct > 50)
		return "images/svg/bt_batt_75.svg";
	if (pct > 25)
		return "images/svg/bt_batt_50.svg";
	return "images/svg/bt_batt_25.svg";
}

void
drop_bt_icon_buffer(void)
{
	if (bt_icon_buf) {
		wlr_buffer_drop(bt_icon_buf);
		bt_icon_buf = NULL;
	}
	bt_icon_loaded_h = 0;
	bt_icon_w = bt_icon_h = 0;
	bt_icon_loaded_path[0] = '\0';
}

int
ensure_bt_icon_buffer(int target_h)
{
	GdkPixbuf *pixbuf = NULL;
	GError *gerr = NULL;
	struct wlr_buffer *buf;
	int w = 0, h = 0;
	char resolved[PATH_MAX];
	const char *path = bt_icon_path;

	if (target_h <= 0)
		return -1;
	if (resolve_asset_path(bt_icon_path, resolved, sizeof(resolved)) == 0
			&& resolved[0])
		path = resolved;
	if (bt_icon_buf && bt_icon_loaded_h == target_h &&
			strncmp(bt_icon_loaded_path, path,
				sizeof(bt_icon_loaded_path)) == 0)
		return 0;
	if (tray_load_svg_pixbuf(path, target_h, &pixbuf) != 0) {
		pixbuf = gdk_pixbuf_new_from_file(path, &gerr);
		if (!pixbuf) {
			if (gerr)
				g_error_free(gerr);
			return -1;
		}
	}
	buf = statusbar_buffer_from_pixbuf(pixbuf, target_h, &w, &h);
	if (!buf)
		return -1;
	drop_bt_icon_buffer();
	bt_icon_buf = buf;
	bt_icon_w = w;
	bt_icon_h = h;
	bt_icon_loaded_h = target_h;
	snprintf(bt_icon_loaded_path, sizeof(bt_icon_loaded_path), "%s", path);
	return 0;
}

void
renderbluetooth(StatusModule *module, int bar_height, const char *text)
{
	BtAdapter a;

	if (!btmon_adapter(&a)) {
		if (module && module->tree) {
			clearstatusmodule(module);
			module->width = 0;
			wlr_scene_node_set_enabled(&module->tree->node, 0);
		}
		return;
	}
	(void)text;
	/* searching = default (powered); disabled variant when the radio
	 * is off — ensure_bt_icon_buffer reloads on path change */
	snprintf(bt_icon_path, sizeof(bt_icon_path), "%s", a.powered ?
			"images/svg/bluetooth_searching.svg" :
			"images/svg/bluetooth_disabled.svg");
	render_tray_icon_module(module, bar_height,
			ensure_bt_icon_buffer, &bt_icon_buf,
			&bt_icon_w, &bt_icon_h);
}

void
render_bt_popup(Monitor *m)
{
	InfoPopup *p;
	Card *card;
	CardResult res;
	BtAdapter a;
	struct timespec ts;
	uint64_t now;
	char v1[96];
	int nconn = 0, i, hot;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	now = (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;

	if (!m || !m->statusbar.bt_popup.tree)
		return;
	p = &m->statusbar.bt_popup;
	if (!statusfont.font || !btmon_adapter(&a)) {
		/* leave visibility to info_popup_hover; a zero size makes the
		 * popup a no-op without fighting its show/hide state */
		p->width = p->height = 0;
		return;
	}

	bt_ui_ndevs = btmon_devices(bt_ui_devs, BT_DEV_MAX);
	for (i = 0; i < bt_ui_ndevs; i++)
		if (bt_ui_devs[i].connected)
			nconn++;

	card = card_begin();
	if (!card)
		return;
	hot = p->btn_hover;

	snprintf(v1, sizeof(v1), "%d", nconn);
	card_header(card, bt_icon_path, "Bluetooth",
			a.powered ? "CONNECTED DEVICES" : "POWERED OFF",
			a.powered ? v1 : "--");
	card_gap(card, 6);

	card_text_btn(card, "Power", NULL, NULL,
			a.powered ? "On" : "Off",
			BT_HIT_POWER, hot == BT_HIT_POWER);
	if (a.powered)
		card_kv2(card, "Adapter", a.name[0] ? a.name : "--", NULL,
				"MAC", a.addr, NULL);

	if (a.powered) {
		int nmine = 0, nnear = 0, pass;

		for (i = 0; i < bt_ui_ndevs; i++) {
			if (bt_ui_devs[i].paired || bt_ui_devs[i].connected)
				nmine++;
			else if (bt_ui_devs[i].name[0])
				nnear++;
		}

		/* pass 0: my (paired/connected) devices with live link
		 * info; pass 1: nearby scan results */
		for (pass = 0; pass < 2; pass++) {
			if (pass == 0 && nmine)
				card_section(card,
						"MY DEVICES · RIGHT-CLICK = FORGET");
			if (pass == 1) {
				card_section(card, "NEARBY");
				card_text_btn(card, "Discovery",
						a.discovering ?
						"Scanning…" : NULL,
						card_col_dim,
						a.discovering ? "Stop" : "Scan",
						BT_HIT_SCAN,
						hot == BT_HIT_SCAN);
			}
			for (i = 0; i < bt_ui_ndevs; i++) {
				BtDev *d = &bt_ui_devs[i];
				const char *btn;
				const char *sig = NULL, *bat = NULL;
				int mine = d->paired || d->connected;

				if ((pass == 0) != mine)
					continue;
				if (!d->name[0] && !mine)
					continue;   /* nameless noise */
				snprintf(v1, sizeof(v1), "%s",
						d->name[0] ? d->name : d->addr);
				if (d->connected) {
					if (d->rssi)
						sig = bt_sig_svg(d->rssi);
					if (d->battery >= 0)
						bat = bt_batt_svg(d->battery);
				}
				if (d->connected)
					btn = !d->svc_resolved ?
						"Connecting.." :
						hot == BT_HIT_DEV + i ?
						"Disconnect" : "Connected";
				else if (d->paired)
					btn = d->want_conn && d->dial_ms &&
						now - d->dial_ms < 5000 ?
						"Connecting.." :
						hot == BT_HIT_DEV + i ?
						"Connect" : "Paired";
				else
					btn = d->pair_ms &&
						now - d->pair_ms < 30000 ?
						"Pairing.." : "Pair";
				if (sig || bat)
					card_icon_text_rbtn_icons(card,
							bt_dev_svg(d), v1,
							sig, bat,
							btn, BT_HIT_DEV + i,
							hot == BT_HIT_DEV + i);
				else
					card_icon_text_rbtn(card,
							bt_dev_svg(d), v1,
							NULL, card_col_dim,
							btn, BT_HIT_DEV + i,
							hot == BT_HIT_DEV + i);
			}
			if (pass == 0 && nmine && nnear)
				card_gap(card, 2);
		}
		if (nconn)
			bt_rssi_ping();
	}

	if (card_finish(card, &res) != 0)
		return;
	memcpy(p->hits, res.hits, sizeof(p->hits));
	p->nhits = res.nhits;
	popup_view_apply(&p->view, p->tree, &res);
	p->width = p->view.w;
	p->height = p->view.h;
}

void
refreshstatusbluetooth(void)
{
	static uint64_t last_visible_ms;
	Monitor *m;
	BtAdapter a;
	BtDev devs[BT_DEV_MAX];
	struct timespec ts;
	uint64_t now;
	int barh, nconn = 0, any_vis = 0, i, n;

	if (btmon_adapter(&a)) {
		n = btmon_devices(devs, BT_DEV_MAX);
		for (i = 0; i < n; i++)
			if (devs[i].connected)
				nconn++;
	}
	wl_list_for_each(m, &mons, link)
		any_vis |= m->statusbar.bt_popup.visible;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	now = (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
	if (any_vis)
		last_visible_ms = now;
	else if (a.discovering && last_visible_ms &&
			now - last_visible_ms > 3000)
		/* nobody is watching the list: stop the scan — inquiry
		 * steals radio slots from live A2DP links (crackle) */
		btmon_set_discovering(0);
	/* change-key: presence, power, connected count */
	snprintf(bt_text, sizeof(bt_text), "%d|%d|%d",
			btmon_adapter(&a), a.powered, nconn);

	wl_list_for_each(m, &mons, link) {
		if (!m->statusbar.bluetooth.tree)
			continue;
		barh = m->statusbar.area.height ?
			m->statusbar.area.height : (int)statusbar_height;
		if (status_should_render(&m->statusbar.bluetooth, barh,
					bt_text)
				|| m->statusbar.bt_popup.visible) {
			renderbluetooth(&m->statusbar.bluetooth, barh,
					bt_text);
			if (m->statusbar.bt_popup.visible)
				render_bt_popup(m);
			positionstatusmodules(m);
		} else if (m->statusbar.bt_popup.visible) {
			render_bt_popup(m);
		}
	}
}

int
bt_popup_handle_click(Monitor *m, int lx, int ly, uint32_t button)
{
	InfoPopup *p = &m->statusbar.bt_popup;
	int rel_x, rel_y, i;

	if (!p->visible || p->width <= 0 || p->height <= 0)
		return 0;
	rel_x = lx - p->tree->node.x;
	rel_y = ly - statusbar_popup_y(m);
	if (rel_x < 0 || rel_y < 0 || rel_x >= p->width || rel_y >= p->height)
		return 0;
	if (button != BTN_LEFT && button != BTN_RIGHT)
		return 1;

	for (i = 0; i < p->nhits; i++) {
		CardHit *hit = &p->hits[i];
		int id;

		if (hit->w <= 0)
			continue;
		if (rel_x < hit->x || rel_x >= hit->x + hit->w ||
				rel_y < hit->y || rel_y >= hit->y + hit->h)
			continue;
		id = hit->id;
		if (id == BT_HIT_POWER && button == BTN_LEFT) {
			BtAdapter a;

			btmon_adapter(&a);
			btmon_set_powered(!a.powered);
		} else if (id == BT_HIT_SCAN && button == BTN_LEFT) {
			BtAdapter a;

			btmon_adapter(&a);
			btmon_set_discovering(!a.discovering);
		} else if (id >= BT_HIT_DEV &&
				id < BT_HIT_DEV + BT_DEV_MAX &&
				id - BT_HIT_DEV < bt_ui_ndevs) {
			BtDev *d = &bt_ui_devs[id - BT_HIT_DEV];

			if (button == BTN_RIGHT)
				btmon_remove(d->path);
			else if (d->connected)
				btmon_disconnect(d->path);
			else if (d->paired)
				btmon_connect(d->path);
			else
				btmon_pair(d->path);
		}
		btsys_changed();
		return 1;
	}
	return 1;
}
