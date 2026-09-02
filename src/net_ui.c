/* Network module + popup: one combined ethernet/wifi/VPN surface.
 * Data comes from netmon (rtnetlink/sysfs/ethtool), wifi_ctrl
 * (wpa_supplicant) and vpnctl (systemd units) — no nmcli, no
 * NetworkManager.  Ethernet with carrier hides wifi (the radio is
 * rfkill-blocked by netmon's policy) but a toggle in the popup can
 * force it back on.  All heavy reads happen only while the popup is
 * open.
 */
#include <limits.h>
#include <math.h>
#include <net/if.h>
#include <stdio.h>
#include <string.h>

#include "nixlytile.h"
#include "netsys.h"
#include "popup_card.h"

/* hit ids in the net popup */
#define NET_HIT_WIFI_TOGGLE 500
#define NET_HIT_SCAN        501   /* Scan Wi-Fi (card_buttons base) */
#define NET_HIT_HIDDEN      502   /* Connect hidden network (base+1) */
#define NET_HIT_BACK        503
#define NET_HIT_CONNECT     504
#define NET_HIT_DISCONNECT2 505   /* CONNECTION-section Disconnect */
#define NET_HIT_SHARE       506
#define NET_HIT_SHARE_COPY  507
#define NET_HIT_FIELD_SSID  508
#define NET_HIT_FIELD_PSK   509
#define NET_HIT_NET_BASE    510   /* + scan index */
#define NET_HIT_VPN_BASE    560   /* + 2*i (even: toggle, odd: auto) */

#define NET_LIST_MAX 24   /* = WIFI_SCAN_MAX: show every network found */
#define VPN_LIST_MAX 6

/* popup view: normal info card, scan list, or hidden-network form */
enum { NETV_NORMAL, NETV_SCAN, NETV_HIDDEN };

static WifiNet ui_nets[WIFI_SCAN_MAX];
static int ui_nnets;
static int ui_scanning;
static uint64_t ui_scan_req_ms;
static uint64_t ui_vpn_refresh_ms;
static NetIfStats ui_stats;
static int ui_stats_ok;
/* status cache: rendernetpopup runs on every hover change, so it must
 * never talk to the supplicant itself — refreshstatusnet (1s cadence
 * while the popup is open) refreshes this */
static WifiStatus ui_ws;
/* rows as last drawn: hit indices resolve against this, not ui_nets,
 * so a rescan between render and click can't retarget a row */
static WifiNet ui_shown[NET_LIST_MAX];
static int ui_nshown;
static char ui_gateway[64];
static char ui_dns[128];
static uint64_t net_prev_stamp_ms;   /* netwatch stamp of net_prev_rx/tx */
static int ui_view;
static char ui_target[33];    /* ssid of the in-flight connect attempt */
static char ui_hid_ssid[33];  /* hidden-network form fields (committed) */
static char ui_hid_psk[64];
static int ui_hid_focus;      /* 0 = SSID field, 1 = passphrase */

static const char lock_closed_icon[] = "images/svg/lock_closed.svg";
static const char lock_open_icon[] = "images/svg/lock_open.svg";

void
netsys_changed(void)
{
	trigger_status_task_now(refreshstatusnet);
}

/* ── text-entry chains ───────────────────────────────────────────── */

static void
scan_psk_submitted(const char *text, void *data)
{
	if (ui_target[0])
		wifi_connect(ui_target, text, 0);
}

static void hid_psk_submitted(const char *text, void *data);

static void
hid_ssid_submitted(const char *text, void *data)
{
	snprintf(ui_hid_ssid, sizeof(ui_hid_ssid), "%s", text);
	ui_hid_focus = 1;
	text_entry_begin("Passphrase", 1, hid_psk_submitted, NULL);
	text_entry_set_text(ui_hid_psk);
}

static void
hid_psk_submitted(const char *text, void *data)
{
	snprintf(ui_hid_psk, sizeof(ui_hid_psk), "%s", text);
	if (ui_hid_ssid[0]) {
		snprintf(ui_target, sizeof(ui_target), "%s", ui_hid_ssid);
		wifi_connect(ui_hid_ssid, ui_hid_psk, 1);
	}
}

/* commit the focused hidden-form field out of the live entry */
static void
hid_capture_focused(void)
{
	if (!text_entry_active())
		return;
	if (ui_hid_focus == 0)
		snprintf(ui_hid_ssid, sizeof(ui_hid_ssid), "%s",
				text_entry_text());
	else
		snprintf(ui_hid_psk, sizeof(ui_hid_psk), "%s",
				text_entry_text());
	text_entry_cancel();
}

static void
net_view_reset(void)
{
	ui_view = NETV_NORMAL;
	ui_target[0] = '\0';
	ui_hid_ssid[0] = '\0';
	memset(ui_hid_psk, 0, sizeof(ui_hid_psk));
	ui_hid_focus = 0;
	if (text_entry_active())
		text_entry_cancel();
}

/* ── data refresh (status task) ──────────────────────────────────── */

static double
dbm_to_pct(int dbm)
{
	double pct = 2.0 * (dbm + 100);

	if (pct < 0.0)
		pct = 0.0;
	if (pct > 100.0)
		pct = 100.0;
	return pct;
}

void
refreshstatusnet(void)
{
	Monitor *m;
	NetLinkSnap s;
	WifiStatus ws;
	int barh, popup_active = 0;
	int wifi_assoc = 0;
	const char *icon_path;
	uint64_t now = monotonic_msec();

	wl_list_for_each(m, &mons, link) {
		if (m->statusbar.net_popup.visible) {
			popup_active = 1;
			break;
		}
	}

	/* the entry only makes sense with the popup on screen; if the popup
	 * was torn down another way, stop swallowing keyboard input */
	if (!popup_active && text_entry_active())
		text_entry_cancel();
	if (!popup_active) {
		if (ui_view != NETV_NORMAL)
			net_view_reset();
		wifi_share_reset();
	}

	netmon_get(&s);
	memset(&ws, 0, sizeof(ws));
	if (s.wifi.present && !s.wifi_blocked && wifi_ctrl_ok()) {
		wifi_status_get(&ws);
		wifi_assoc = strcmp(ws.state, "COMPLETED") == 0;
	}
	ui_ws = ws;

	/* radio gone → the scan/hidden views have nothing to stand on */
	if (ui_view != NETV_NORMAL && (!s.wifi.present || s.wifi_blocked))
		net_view_reset();
	/* successful connect from scan/hidden → back to the normal card,
	 * now showing the new network */
	if (ui_view != NETV_NORMAL && ui_target[0] && wifi_assoc &&
			strcmp(ws.ssid, ui_target) == 0)
		net_view_reset();

	net_available = (s.eth.present && s.eth.carrier) || wifi_assoc;
	net_is_wireless = !(s.eth.present && s.eth.carrier) && wifi_assoc;

	if (!net_available) {
		icon_path = net_icon_no_conn_resolved[0] ?
			net_icon_no_conn_resolved : net_icon_no_conn;
		net_iface[0] = '\0';
		snprintf(net_local_ip, sizeof(net_local_ip), "--");
		snprintf(net_ssid, sizeof(net_ssid), "--");
		net_last_wifi_quality = -1.0;
		net_link_speed_mbps = -1;
		net_last_down_bps = net_last_up_bps = -1.0;
		snprintf(net_down_text, sizeof(net_down_text), "--");
		snprintf(net_up_text, sizeof(net_up_text), "--");
		net_prev_valid = 0;
	} else if (net_is_wireless) {
		snprintf(net_iface, sizeof(net_iface), "%s", s.wifi.iface);
		snprintf(net_ssid, sizeof(net_ssid), "%s",
				ws.ssid[0] ? ws.ssid : "WiFi");
		net_last_wifi_quality = dbm_to_pct(ws.signal_dbm);
		net_link_speed_mbps = ws.link_speed_mbps;
		icon_path = wifi_icon_for_quality(net_last_wifi_quality);
	} else {
		snprintf(net_iface, sizeof(net_iface), "%s", s.eth.iface);
		snprintf(net_ssid, sizeof(net_ssid), "Ethernet");
		net_last_wifi_quality = -1.0;
		net_link_speed_mbps = s.eth.speed_mbps;
		icon_path = net_icon_eth_resolved[0] ?
			net_icon_eth_resolved : net_icon_eth;
	}

	/* change-key for the bar module (never drawn): link identity +
	 * signal bucket so icon changes trigger a re-render */
	snprintf(net_text, sizeof(net_text), "%d|%.20s|%d|%d|%d",
			net_is_wireless, net_iface, s.wifi_blocked,
			net_last_wifi_quality >= 0.0 ?
			(int)(net_last_wifi_quality / 25.0) : -1,
			net_available);

	if (net_available) {
		if (strncmp(net_prev_iface, net_iface,
					sizeof(net_prev_iface)) != 0) {
			net_prev_valid = 0;
			snprintf(net_prev_iface, sizeof(net_prev_iface),
					"%s", net_iface);
		}
		if (!localip(net_iface, net_local_ip, sizeof(net_local_ip)))
			snprintf(net_local_ip, sizeof(net_local_ip), "--");
	}

	if (popup_active && net_available) {
		request_public_ip_async_ex(1);
		netmon_gateway(ui_gateway, sizeof(ui_gateway));
		netmon_dns(ui_dns, sizeof(ui_dns));
		ui_stats_ok = netmon_stats(net_iface, &ui_stats) == 0;

		if (!ui_stats_ok) {
			net_last_down_bps = net_last_up_bps = -1.0;
			snprintf(net_down_text, sizeof(net_down_text), "--");
			snprintf(net_up_text, sizeof(net_up_text), "--");
		} else if (!net_prev_valid ||
				ui_stats.stamp_ms != net_prev_stamp_ms) {
			/* rate only across two distinct worker samples: the
			 * snapshot can repeat between 1s ticks, and a zero
			 * delta would flash the rate to 0 */
			double elapsed = net_prev_valid ?
				(ui_stats.stamp_ms - net_prev_stamp_ms) /
				1000.0 : 0.0;

			net_last_down_bps = net_prev_valid ?
				net_bytes_to_rate(ui_stats.rx_bytes,
						net_prev_rx, elapsed) : -1.0;
			net_last_up_bps = net_prev_valid ?
				net_bytes_to_rate(ui_stats.tx_bytes,
						net_prev_tx, elapsed) : -1.0;
			format_speed(net_last_down_bps, net_down_text,
					sizeof(net_down_text));
			format_speed(net_last_up_bps, net_up_text,
					sizeof(net_up_text));
			net_prev_rx = ui_stats.rx_bytes;
			net_prev_tx = ui_stats.tx_bytes;
			net_prev_stamp_ms = ui_stats.stamp_ms;
			net_prev_valid = 1;
		}
	}

	if (popup_active && s.wifi.present && !s.wifi_blocked &&
			wifi_ctrl_ok()) {
		ui_nnets = wifi_scan_get(ui_nets, WIFI_SCAN_MAX);
		if (ui_nnets > 0 && ui_scanning &&
				now - ui_scan_req_ms > 3000)
			ui_scanning = 0;
		/* keep results fresh while the popup is open */
		if (now - ui_scan_req_ms > 30000) {
			wifi_scan_request();
			ui_scan_req_ms = now;
		}
	}
	if (popup_active && now - ui_vpn_refresh_ms > 10000) {
		vpnctl_refresh();
		ui_vpn_refresh_ms = now;
	}

	set_net_icon_path(icon_path);

	wl_list_for_each(m, &mons, link) {
		if (!m->statusbar.net.tree)
			continue;
		barh = m->statusbar.area.height ?
			m->statusbar.area.height : (int)statusbar_height;
		if (status_should_render(&m->statusbar.net, barh, net_text)
				|| m->statusbar.net_popup.visible) {
			rendernet(&m->statusbar.net, barh, net_text);
			if (m->statusbar.net_popup.visible)
				rendernetpopup(m);
			positionstatusmodules(m);
		} else if (m->statusbar.net_popup.visible) {
			rendernetpopup(m);
		}
	}
}

/* ── popup card ──────────────────────────────────────────────────── */

/* CONNECTION-footer buttons that enter the scan / hidden views */
static void
scan_hidden_buttons(Card *card, int hot)
{
	const char *lbl[2] = { "Scan Wi-Fi", "Connect hidden network" };

	card_gap(card, 4);
	card_buttons(card, lbl, NULL, 2, -1,
			hot == NET_HIT_SCAN ? 0 :
			hot == NET_HIT_HIDDEN ? 1 : -1, NET_HIT_SCAN);
}

/* Scan view: header + every found network as a BT-style hover row —
 * signal icon, SSID, security method, open/closed padlock.  The picked
 * row turns into a passphrase entry with a Connect button. */
static void
render_scan_view(Card *card, int hot)
{
	int i;

	card_section(card, "NETWORKS");
	card_text_rbtn(card, "Select a network",
			ui_scanning ? "Scanning…" : NULL, card_col_dim,
			"Back", NET_HIT_BACK, hot == NET_HIT_BACK);
	if (wifi_last_error()[0])
		card_text(card, wifi_last_error(), NULL, card_col_red);
	ui_nshown = ui_nnets < NET_LIST_MAX ? ui_nnets : NET_LIST_MAX;
	if (!ui_nshown) {
		card_gap(card, 8);
		card_loading(card, "SCANNING",
				(double)(monotonic_msec() % 2400) / 2400.0);
		card_gap(card, 8);
		return;
	}
	for (i = 0; i < ui_nshown; i++) {
		WifiNet *w = &ui_shown[i];
		const char *sec;

		*w = ui_nets[i];
		if (ui_target[0] && text_entry_active() &&
				strcmp(w->ssid, ui_target) == 0) {
			card_text_rbtn(card, w->ssid,
					text_entry_display(), card_col_blue,
					"Connect", NET_HIT_CONNECT,
					hot == NET_HIT_CONNECT);
			continue;
		}
		sec = w->connected ? "Connected" :
			(w->secured ? (w->sec[0] ? w->sec : "WPA") : "Open");
		card_icon_text_hit(card,
				wifi_icon_for_quality(dbm_to_pct(w->signal_dbm)),
				w->ssid, sec,
				w->connected || w->known ?
				card_col_green : card_col_dim,
				w->secured ? lock_closed_icon : lock_open_icon,
				NET_HIT_NET_BASE + i,
				hot == NET_HIT_NET_BASE + i);
	}
}

/* Hidden-network view: SSID + passphrase fields stacked, Connect below */
static void
render_hidden_view(Card *card, int hot)
{
	static char pdots[4 * 64];
	const char *lbl[1] = { "Connect" };
	const char *sd, *pd;
	size_t i, n;

	card_section(card, "HIDDEN NETWORK");
	card_text_rbtn(card, "Enter network details", NULL, NULL,
			"Back", NET_HIT_BACK, hot == NET_HIT_BACK);
	sd = ui_hid_focus == 0 && text_entry_active() ?
		text_entry_display() : ui_hid_ssid;
	if (ui_hid_focus == 1 && text_entry_active()) {
		pd = text_entry_display();
	} else {
		n = strlen(ui_hid_psk);
		pdots[0] = '\0';
		for (i = 0; i < n; i++)
			strcat(pdots, "•");
		pd = pdots;
	}
	card_icon_text_hit(card, NULL, "SSID", sd,
			ui_hid_focus == 0 && text_entry_active() ?
			card_col_blue : card_col_fg,
			NULL, NET_HIT_FIELD_SSID, hot == NET_HIT_FIELD_SSID);
	card_icon_text_hit(card, NULL, "Passphrase", pd,
			ui_hid_focus == 1 && text_entry_active() ?
			card_col_blue : card_col_fg,
			NULL, NET_HIT_FIELD_PSK, hot == NET_HIT_FIELD_PSK);
	if (wifi_last_error()[0])
		card_text(card, wifi_last_error(), NULL, card_col_red);
	card_gap(card, 4);
	card_buttons(card, lbl, NULL, 1, -1,
			hot == NET_HIT_CONNECT ? 0 : -1, NET_HIT_CONNECT);
}

/* ── live spectrum overlay (wifi header) ─────────────────────────── */

#define SPEC_TICK_MS 33

static struct wlr_scene_buffer *spec_node;   /* child of view content;
                                              * reset on every re-render */
static int spec_x, spec_y, spec_w, spec_h;
static double spec_frac;
static const float *spec_accent;
static struct wl_event_source *spec_timer;
/* Ping-pong frame buffers: the 33ms tick used to route every frame
 * through card_spectrum_buffer (cairo surface create + ecalloc + full
 * copy + wlr buffer create, then drop).  Instead two persistent
 * pixman-backed wlr_buffers are drawn in place with cairo and reused
 * across ticks; two alternate so a frame the scene may still be
 * presenting is never scribbled.  Dropped when the overlay closes. */
static struct PixmanBuffer *spec_bufs[2];
static int spec_buf_flip;

#define SPEC_PI 3.14159265358979323846

static void
spec_bufs_drop(void)
{
	int i;

	for (i = 0; i < 2; i++) {
		if (spec_bufs[i]) {
			wlr_buffer_drop(&spec_bufs[i]->base);
			spec_bufs[i] = NULL;
		}
	}
}

static struct PixmanBuffer *
spec_buf_acquire(int w, int h)
{
	struct PixmanBuffer *b = spec_bufs[spec_buf_flip];

	if (b && (b->base.width != w || b->base.height != h)) {
		spec_bufs_drop();
		b = NULL;
	}
	if (!b) {
		int stride = w * 4;
		void *data = ecalloc(1, (size_t)stride * (size_t)h);

		b = ecalloc(1, sizeof(*b));
		b->image = pixman_image_create_bits(PIXMAN_a8r8g8b8, w, h,
				data, stride);
		b->data = data;
		b->drm_format = DRM_FORMAT_ARGB8888;
		b->stride = stride;
		b->owns_data = 1;
		wlr_buffer_init(&b->base, &pixman_buffer_impl, w, h);
		spec_bufs[spec_buf_flip] = b;
	}
	spec_buf_flip ^= 1;
	return b;
}

/* Per-bar level — mirrors popup_card.c's card_spectrum_buffer: two
 * incommensurate sines beat against each other so the motion reads
 * organic, never a marching wave. */
static double
spec_level(double ph, int i)
{
	double v = 0.5 + 0.30 * sin(ph * 2.1 + i * 0.83) +
			0.24 * sin(ph * 3.7 + i * 1.94 + 1.7);

	return v < 0.0 ? 0.0 : v > 1.0 ? 1.0 : v;
}

static void
spec_rounded(cairo_t *cr, double x, double y, double w, double h, double r)
{
	cairo_new_sub_path(cr);
	cairo_arc(cr, x + w - r, y + r, r, -SPEC_PI / 2, 0);
	cairo_arc(cr, x + w - r, y + h - r, r, 0, SPEC_PI / 2);
	cairo_arc(cr, x + r, y + h - r, r, SPEC_PI / 2, SPEC_PI);
	cairo_arc(cr, x + r, y + r, r, SPEC_PI, 3 * SPEC_PI / 2);
	cairo_close_path(cr);
}

/* (Re)create the overlay node and swap in a freshly drawn frame. Safe
 * to call right after a card re-render so the spectrum never drops out
 * for a frame. */
static void
spec_overlay_draw(NetPopup *p)
{
	struct PixmanBuffer *pb;
	cairo_surface_t *cs;
	cairo_t *cr;
	const int bar_w = 3, gap = 2;
	int nbars, i;
	double base, frac, t;

	/* Wait out the card show animation: extra children don't take part
	 * in its fade, so the spectrum joins once the card has settled. */
	if (spec_w <= 0 || !p->view.content || p->view.animating)
		return;
	if (!spec_node) {
		spec_node = wlr_scene_buffer_create(p->view.content, NULL);
		if (spec_node)
			wlr_scene_node_set_position(&spec_node->node,
					spec_x, spec_y);
	}
	if (!spec_node)
		return;
	pb = spec_buf_acquire(spec_w, spec_h);
	cs = cairo_image_surface_create_for_data(pb->data,
			CAIRO_FORMAT_ARGB32, spec_w, spec_h, pb->stride);
	if (cairo_surface_status(cs) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(cs);
		return;
	}
	cr = cairo_create(cs);
	cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
	cairo_paint(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

	/* same frame as popup_card.c's card_spectrum_buffer: bottom-aligned
	 * bars tapering toward the high end with peak-hold caps, envelope
	 * and speed scaling with signal strength */
	nbars = (spec_w + gap) / (bar_w + gap);
	base = spec_h - 1.0;
	frac = spec_frac < 0.0 ? 0.0 : spec_frac > 1.0 ? 1.0 : spec_frac;
	t = monotonic_msec() / 1000.0;
	for (i = 0; i < nbars; i++) {
		double ph = t * (1.2 + 1.0 * frac);
		double tilt = 1.0 - 0.45 * i / (nbars > 1 ? nbars - 1 : 1);
		double env = (0.2 + 0.8 * frac) * tilt * (spec_h - 5.0);
		double v = spec_level(ph, i);
		double x = i * (bar_w + gap);
		double bh = 2.0 + env * v;
		double peak = v;
		int s;

		spec_rounded(cr, x, base - bh, bar_w, bh, 1.5);
		cairo_set_source_rgba(cr, spec_accent[0], spec_accent[1],
				spec_accent[2],
				spec_accent[3] * (0.30 + 0.70 * v));
		cairo_fill(cr);

		/* peak-hold cap: max level over the recent past, floating
		 * just above the bar and decaying as the bar falls away */
		for (s = 1; s <= 4; s++) {
			double pv = spec_level(ph - s * 0.09, i);

			if (pv > peak)
				peak = pv;
		}
		cairo_rectangle(cr, x, base - (2.0 + env * peak) - 3.0,
				bar_w, 1.5);
		cairo_set_source_rgba(cr, spec_accent[0], spec_accent[1],
				spec_accent[2], spec_accent[3] * 0.85);
		cairo_fill(cr);
	}
	cairo_destroy(cr);
	cairo_surface_flush(cs);
	cairo_surface_destroy(cs);
	wlr_scene_buffer_set_buffer(spec_node, &pb->base);
}

/* ~30 fps heartbeat; stops itself the moment no net popup is visible
 * or the header has no spectrum row (ethernet / disconnected). */
static int
spec_tick(void *data)
{
	Monitor *m, *pm = NULL;
	NetPopup *p = NULL;

	(void)data;
	wl_list_for_each(m, &mons, link) {
		if (m->statusbar.net_popup.visible) {
			p = &m->statusbar.net_popup;
			pm = m;
			break;
		}
	}
	if (!p || spec_w <= 0) {
		spec_node = NULL;
		spec_bufs_drop();
		return 0;   /* stays disarmed until the next popup render */
	}
	spec_overlay_draw(p);
	if (pm->wlr_output)
		wlr_output_schedule_frame(pm->wlr_output);
	wl_event_source_timer_update(spec_timer, SPEC_TICK_MS);
	return 0;
}

static void
spec_timer_arm(void)
{
	if (!spec_timer)
		spec_timer = wl_event_loop_add_timer(event_loop, spec_tick,
				NULL);
	if (spec_timer)
		wl_event_source_timer_update(spec_timer, SPEC_TICK_MS);
}

void
rendernetpopup(Monitor *m)
{
	NetPopup *p;
	Card *card;
	CardResult res;
	NetLinkSnap s;
	WifiStatus ws;
	char value[32], sub[64], v1[64], v2[64];
	int wifi_assoc = 0, hot, i;
	size_t si;

	if (!m || !m->statusbar.net_popup.tree)
		return;
	p = &m->statusbar.net_popup;
	if (!statusfont.font) {
		p->width = p->height = 0;
		wlr_scene_node_set_enabled(&p->tree->node, 0);
		p->visible = 0;
		return;
	}

	netmon_get(&s);
	ws = ui_ws;   /* cached by refreshstatusnet; no supplicant I/O here */
	wifi_assoc = ws.active && strcmp(ws.state, "COMPLETED") == 0;

	card = card_begin();
	if (!card)
		return;
	card_at(m, m->statusbar.area.x + p->anchor_x,
			m->statusbar.area.y + statusbar_popup_y(m));
	hot = p->btn_hover;

	/* header */
	if (s.eth.present && s.eth.carrier) {
		if (s.eth.speed_mbps >= 1000)
			snprintf(value, sizeof(value), "%.1fG",
					s.eth.speed_mbps / 1000.0);
		else if (s.eth.speed_mbps > 0)
			snprintf(value, sizeof(value), "%dM", s.eth.speed_mbps);
		else
			snprintf(value, sizeof(value), "--");
		card_header(card, net_icon_path, "Ethernet", "WIRED LINK",
				value);
		card_gap(card, 8);
	} else if (wifi_assoc) {
		int pct = (int)lround(dbm_to_pct(ws.signal_dbm));

		snprintf(value, sizeof(value), "%d%%", pct);
		snprintf(sub, sizeof(sub), "%s", ws.ssid);
		for (si = 0; sub[si]; si++)
			sub[si] = (char)toupper((unsigned char)sub[si]);
		card_header(card, net_icon_path, "Wi-Fi", sub, value);
		card_gap(card, 8);
	} else {
		card_header(card, net_icon_path, "Network", "DISCONNECTED",
				"--");
		card_gap(card, 8);
	}

	/* scan / hidden views replace everything below the header */
	if (ui_view != NETV_NORMAL) {
		if (ui_view == NETV_SCAN)
			render_scan_view(card, hot);
		else
			render_hidden_view(card, hot);
		goto finish;
	}

	if (net_available && strcmp(net_local_ip, "--") == 0 &&
			!ui_gateway[0] &&
			strcmp(net_public_ip, "--") == 0 &&
			net_last_down_bps < 0.0 && net_last_up_bps < 0.0) {
		/* link up but nothing fetched yet (DHCP/first tick):
		 * spinner instead of a wall of "--" rows */
		card_gap(card, 10);
		card_loading(card, "LOADING DATA",
				(double)(monotonic_msec() % 2400) / 2400.0);
		card_gap(card, 10);
	} else if (net_available) {
		/* left column: Local IP / Public IP / Gateway stacked;
		 * right column: DNS with one server per row */
		char dbuf[128], *tok, *save;
		const char *dns[6];
		const char *lk[3] = { "Local IP", "Public IP", "Gateway" };
		const char *lv[3];
		int ndns = 0, nrows, row;

		lv[0] = net_local_ip[0] ? net_local_ip : "--";
		lv[1] = net_public_ip[0] ? net_public_ip : "--";
		lv[2] = ui_gateway[0] ? ui_gateway : "--";
		snprintf(dbuf, sizeof(dbuf), "%s", ui_dns);
		for (tok = strtok_r(dbuf, ", ", &save);
				tok && ndns < (int)LENGTH(dns);
				tok = strtok_r(NULL, ", ", &save))
			dns[ndns++] = tok;
		nrows = ndns > 3 ? ndns : 3;
		for (row = 0; row < nrows; row++)
			card_kv2(card,
					row < 3 ? lk[row] : "",
					row < 3 ? lv[row] : "",
					NULL,
					row < ndns ? (row ? "" : "DNS") : NULL,
					row < ndns ? dns[row] : NULL,
					NULL);

		card_section(card, "THROUGHPUT");
		card_kv2(card, "Upload",
				net_up_text[0] ? net_up_text : "--",
				card_col_green, "Download",
				net_down_text[0] ? net_down_text : "--",
				card_col_blue);
		if (ui_stats_ok && (ui_stats.rx_errors || ui_stats.tx_errors ||
				ui_stats.rx_dropped || ui_stats.tx_dropped)) {
			snprintf(v1, sizeof(v1), "%llu / %llu",
					ui_stats.rx_errors, ui_stats.tx_errors);
			snprintf(v2, sizeof(v2), "%llu / %llu",
					ui_stats.rx_dropped,
					ui_stats.tx_dropped);
			card_kv2(card, "Errors rx/tx", v1, card_col_yellow,
					"Drops", v2, card_col_yellow);
		}
	}

	/* ethernet link details */
	if (s.eth.present && s.eth.carrier) {
		card_section(card, "ETHERNET");
		if (s.eth.speed_mbps > 0)
			snprintf(v1, sizeof(v1), "%d Mbit/s", s.eth.speed_mbps);
		else
			snprintf(v1, sizeof(v1), "--");
		card_kv2(card, "Link", v1, card_col_green, "Duplex",
				s.eth.duplex_full < 0 ? "--" :
				(s.eth.duplex_full ? "Full" : "Half"), NULL);
		card_kv2(card, "Iface", s.eth.iface, NULL, "MAC", s.eth.mac,
				NULL);
	}

	/* wifi */
	card_section(card, "WI-FI");
	if (!s.wifi.present) {
		card_text(card, "No wifi adapter", NULL, NULL);
	} else {
		card_big_btn(card,
				s.wifi_blocked ? "Radio OFF" : "Radio ON",
				s.wifi_blocked ? card_col_red : card_col_green,
				NET_HIT_WIFI_TOGGLE,
				hot == NET_HIT_WIFI_TOGGLE);
	}
	if (s.wifi.present && !s.wifi_blocked) {
		if (wifi_assoc) {
			card_section(card, "CONNECTION");
			card_text_btn2(card, ws.ssid,
					"Disconnect", NET_HIT_DISCONNECT2,
					hot == NET_HIT_DISCONNECT2,
					"Share Wi-Fi", NET_HIT_SHARE,
					hot == NET_HIT_SHARE);
			snprintf(v1, sizeof(v1), "%d dBm · %.1f GHz",
					ws.signal_dbm, ws.freq_mhz / 1000.0);
			card_kv2(card, "Signal", v1, NULL,
					"BSSID", ws.bssid, NULL);
			if (ws.link_speed_mbps > 0) {
				snprintf(v1, sizeof(v1), "%d Mbit/s",
						ws.link_speed_mbps);
				snprintf(v2, sizeof(v2), "%d Mbit/s",
						ws.max_rate_mbps);
				card_kv2(card, "Link rate", v1, card_col_green,
						"Max seen", v2, NULL);
			}
			card_kv2(card, "Security",
					ws.key_mgmt[0] ? ws.key_mgmt : "Open",
					NULL, NULL, NULL, NULL);
			scan_hidden_buttons(card, hot);
			if (wifi_share_active() &&
					strcmp(wifi_share_ssid(), ws.ssid) != 0)
				wifi_share_reset();
			if (wifi_share_active()) {
				int qs;
				const uint8_t *qm = wifi_share_qr(&qs);

				card_section(card, "SHARE WI-FI");
				if (qm) {
					card_gap(card, 4);
					card_qr(card, qm, qs);
					card_gap(card, 4);
					card_text_btn(card,
							"Scan to join this network",
							NULL, NULL, "Copy image",
							NET_HIT_SHARE_COPY,
							hot == NET_HIT_SHARE_COPY);
				} else {
					card_text(card, wifi_share_status(),
							NULL, card_col_red);
				}
			}
		} else {
			if (wifi_last_error()[0])
				card_text(card, wifi_last_error(), NULL,
						card_col_red);
			scan_hidden_buttons(card, hot);
		}
	}

	/* VPN */
	{
		VpnProfile prof[VPN_MAX];
		int nprof = vpnctl_profiles(prof, VPN_MAX);

		if (nprof > 0) {
			card_section(card, "VPN");
			for (i = 0; i < nprof && i < VPN_LIST_MAX; i++) {
				VpnProfile *v = &prof[i];
				int tid = NET_HIT_VPN_BASE + 2 * i;
				int aid = tid + 1;

				card_text_btn(card, v->label,
						v->busy ? "…" :
						(v->active ? "Active" : NULL),
						v->active ? card_col_green :
						card_col_dim,
						v->active ? "Stop" : "Start",
						tid, hot == tid);
				card_kv2_btn(card, "", "", NULL,
						"Autoconnect",
						v->autoconnect ? "On" : "Off",
						v->autoconnect ?
						card_col_green : NULL,
						aid, hot == aid);
			}
		}
	}

finish:
	if (card_finish(card, &res) != 0)
		return;
	memcpy(p->hits, res.hits, sizeof(p->hits));
	p->nhits = res.nhits;
	popup_view_apply(&p->view, p->tree, &res);
	p->width = p->view.w;
	p->height = p->view.h;

	/* apply destroyed the previous overlay with the old card content;
	 * redraw right away so the spectrum never blinks out for a frame */
	spec_node = NULL;
	spec_x = res.wave_x;
	spec_y = res.wave_y;
	spec_w = res.wave_w;
	spec_h = res.wave_h;
	if (spec_w > 0) {
		spec_overlay_draw(p);
		spec_timer_arm();
	}
}

/* ── hover + clicks ──────────────────────────────────────────────── */

/* Track which hit is under the cursor; re-render on change.  Called
 * from updatenethover once the popup is visible. */
void
net_popup_track_hover(Monitor *m, double cx, double cy)
{
	NetPopup *p = &m->statusbar.net_popup;
	int rel_x, rel_y, new_hover = -1, i;

	if (!p->visible)
		return;
	rel_x = (int)cx - (m->statusbar.area.x + p->anchor_x);
	rel_y = (int)cy - (m->statusbar.area.y + statusbar_popup_y(m));
	if (rel_x >= 0 && rel_y >= 0 && rel_x < p->width &&
			rel_y < p->height) {
		for (i = 0; i < p->nhits; i++) {
			CardHit *hit = &p->hits[i];

			if (hit->w <= 0)
				continue;
			if (rel_x >= hit->x && rel_x < hit->x + hit->w &&
					rel_y >= hit->y &&
					rel_y < hit->y + hit->h) {
				new_hover = hit->id;
				break;
			}
		}
	}
	if (new_hover != p->btn_hover) {
		p->btn_hover = new_hover;
		rendernetpopup(m);
	}
}

int
net_popup_handle_click(Monitor *m, int lx, int ly, uint32_t button)
{
	NetPopup *p = &m->statusbar.net_popup;
	int rel_x, rel_y, i;

	if (!p->visible || p->width <= 0)
		return 0;
	rel_x = lx - p->anchor_x;
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

		if (id == NET_HIT_WIFI_TOGGLE && button == BTN_LEFT) {
			NetLinkSnap s;

			netmon_get(&s);
			netmon_wifi_set_enabled(s.wifi_blocked, 1);
		} else if (id == NET_HIT_SCAN && button == BTN_LEFT) {
			ui_view = NETV_SCAN;
			ui_target[0] = '\0';
			if (text_entry_active())
				text_entry_cancel();
			wifi_scan_request();
			ui_scanning = 1;
			ui_scan_req_ms = monotonic_msec();
		} else if (id == NET_HIT_HIDDEN && button == BTN_LEFT) {
			ui_view = NETV_HIDDEN;
			ui_target[0] = '\0';
			ui_hid_ssid[0] = '\0';
			memset(ui_hid_psk, 0, sizeof(ui_hid_psk));
			ui_hid_focus = 0;
			text_entry_begin("SSID", 0, hid_ssid_submitted, NULL);
		} else if (id == NET_HIT_BACK && button == BTN_LEFT) {
			net_view_reset();
		} else if (id == NET_HIT_CONNECT && button == BTN_LEFT) {
			if (ui_view == NETV_SCAN && ui_target[0]) {
				char psk[80];

				snprintf(psk, sizeof(psk), "%s",
						text_entry_text());
				if (text_entry_active())
					text_entry_cancel();
				wifi_connect(ui_target, psk, 0);
				memset(psk, 0, sizeof(psk));
			} else if (ui_view == NETV_HIDDEN) {
				hid_capture_focused();
				if (ui_hid_ssid[0]) {
					snprintf(ui_target, sizeof(ui_target),
							"%s", ui_hid_ssid);
					wifi_connect(ui_hid_ssid,
							ui_hid_psk, 1);
				}
			}
		} else if (id == NET_HIT_FIELD_SSID && button == BTN_LEFT) {
			hid_capture_focused();
			ui_hid_focus = 0;
			text_entry_begin("SSID", 0, hid_ssid_submitted, NULL);
			text_entry_set_text(ui_hid_ssid);
		} else if (id == NET_HIT_FIELD_PSK && button == BTN_LEFT) {
			hid_capture_focused();
			ui_hid_focus = 1;
			text_entry_begin("Passphrase", 1, hid_psk_submitted,
					NULL);
			text_entry_set_text(ui_hid_psk);
		} else if (id == NET_HIT_DISCONNECT2 && button == BTN_LEFT) {
			wifi_disconnect();
		} else if (id == NET_HIT_SHARE && button == BTN_LEFT) {
			wifi_share_toggle(&ui_ws);
		} else if (id == NET_HIT_SHARE_COPY && button == BTN_LEFT) {
			wifi_share_copy();
		} else if (id >= NET_HIT_NET_BASE &&
				id < NET_HIT_NET_BASE + NET_LIST_MAX) {
			WifiNet *w = &ui_shown[id - NET_HIT_NET_BASE];

			if (id - NET_HIT_NET_BASE >= ui_nshown) {
				/* stale list */
			} else if (button == BTN_RIGHT) {
				if (w->known)
					wifi_forget(w->known_id);
			} else if (w->connected) {
				/* already on this network */
			} else if (w->known) {
				snprintf(ui_target, sizeof(ui_target), "%s",
						w->ssid);
				wifi_connect_known(w->known_id);
			} else if (w->secured) {
				/* row becomes a passphrase entry + Connect */
				snprintf(ui_target, sizeof(ui_target), "%s",
						w->ssid);
				text_entry_begin("Passphrase", 1,
						scan_psk_submitted, NULL);
			} else {
				snprintf(ui_target, sizeof(ui_target), "%s",
						w->ssid);
				wifi_connect(w->ssid, "", 0);
			}
		} else if (id >= NET_HIT_VPN_BASE &&
				id < NET_HIT_VPN_BASE + 2 * VPN_LIST_MAX &&
				button == BTN_LEFT) {
			int idx = (id - NET_HIT_VPN_BASE) / 2;

			if ((id - NET_HIT_VPN_BASE) % 2 == 0) {
				vpnctl_toggle(idx);
			} else {
				VpnProfile prof[VPN_MAX];
				int nprof = vpnctl_profiles(prof, VPN_MAX);

				if (idx < nprof)
					vpnctl_set_autoconnect(idx,
							!prof[idx].autoconnect);
			}
		}
		netsys_changed();
		return 1;
	}
	return 1;   /* swallow clicks on the card body */
}
