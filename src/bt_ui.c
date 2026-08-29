/* Bluetooth statusbar module + popup (replaces blueman).  Pure view on
 * btmon's model: power toggle, discovery, pair/connect/forget, device
 * battery.  Left-click a device = pair or connect/disconnect;
 * right-click = forget.
 */
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "nixlytile.h"
#include "netsys.h"
#include "popup_card.h"

#define BT_HIT_POWER 210
#define BT_HIT_SCAN  211
#define BT_HIT_DEV   220   /* + device index */

char bt_icon_path[PATH_MAX] = "images/svg/bluetooth.svg";
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
	char v1[96], v2[64];
	int nconn = 0, i, hot;

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

	card_kv2_btn(card, "Power", a.powered ? "On" : "Off",
			a.powered ? card_col_green : NULL,
			"Toggle", a.powered ? "Disable" : "Enable", NULL,
			BT_HIT_POWER, hot == BT_HIT_POWER);
	if (a.powered)
		card_kv2(card, "Adapter", a.name[0] ? a.name : "--", NULL,
				"MAC", a.addr, NULL);

	if (a.powered) {
		card_section(card, "DEVICES · RIGHT-CLICK = FORGET");
		card_text_btn(card, "Discovery",
				a.discovering ? "Scanning…" : NULL,
				card_col_dim,
				a.discovering ? "Stop" : "Scan",
				BT_HIT_SCAN, hot == BT_HIT_SCAN);
		for (i = 0; i < bt_ui_ndevs; i++) {
			BtDev *d = &bt_ui_devs[i];
			const char *btn;
			size_t pos;

			if (!d->name[0] && !d->paired)
				continue;   /* nameless broadcast noise */
			pos = snprintf(v1, sizeof(v1), "%s",
					d->name[0] ? d->name : d->addr);
			if (d->battery >= 0 && pos < sizeof(v1) - 8)
				snprintf(v1 + pos, sizeof(v1) - pos,
						" · %d%%", d->battery);
			if (d->connected)
				snprintf(v2, sizeof(v2), "Connected");
			else if (d->paired)
				snprintf(v2, sizeof(v2), "Paired");
			else if (d->rssi)
				snprintf(v2, sizeof(v2), "%d dBm", d->rssi);
			else
				v2[0] = '\0';
			btn = d->connected ? "Disc" :
				(d->paired ? "Conn" : "Pair");
			card_text_btn(card, v1, v2[0] ? v2 : NULL,
					d->connected ? card_col_green :
					card_col_dim,
					btn, BT_HIT_DEV + i,
					hot == BT_HIT_DEV + i);
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

void
refreshstatusbluetooth(void)
{
	Monitor *m;
	BtAdapter a;
	BtDev devs[BT_DEV_MAX];
	int barh, nconn = 0, i, n;

	if (btmon_adapter(&a)) {
		n = btmon_devices(devs, BT_DEV_MAX);
		for (i = 0; i < n; i++)
			if (devs[i].connected)
				nconn++;
	}
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
