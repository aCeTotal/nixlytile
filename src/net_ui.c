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
#include <time.h>

#include "nixlytile.h"
#include "netsys.h"
#include "popup_card.h"

/* hit ids in the net popup */
#define NET_HIT_WIFI_TOGGLE 500
#define NET_HIT_SCAN        501
#define NET_HIT_HIDDEN      502
#define NET_HIT_DISCONNECT  503
#define NET_HIT_SEARCH      504
#define NET_HIT_NET_BASE    510   /* + scan index */
#define NET_HIT_VPN_BASE    560   /* + 2*i (even: toggle, odd: auto) */

#define NET_LIST_MAX 24   /* = WIFI_SCAN_MAX: show every network found */
#define NET_LIST_FOLD 12  /* rows shown without a search filter */
#define VPN_LIST_MAX 6

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
static char ui_pend_ssid[33];
static int ui_pend_hidden;
static char ui_search[33];

static const char search_label[] = "Search networks";

void
netsys_changed(void)
{
	trigger_status_task_now(refreshstatusnet);
}

/* ── text-entry chains ───────────────────────────────────────────── */

static void
psk_submitted(const char *text, void *data)
{
	if (ui_pend_ssid[0])
		wifi_connect(ui_pend_ssid, text, ui_pend_hidden);
	memset(ui_pend_ssid, 0, sizeof(ui_pend_ssid));
}

static void
hidden_ssid_submitted(const char *text, void *data)
{
	char label[96];

	if (!text[0])
		return;
	snprintf(ui_pend_ssid, sizeof(ui_pend_ssid), "%s", text);
	ui_pend_hidden = 1;
	snprintf(label, sizeof(label), "Password for %s (empty = open)",
			ui_pend_ssid);
	text_entry_begin(label, 1, psk_submitted, NULL);
}

static void
search_submitted(const char *text, void *data)
{
	snprintf(ui_search, sizeof(ui_search), "%s", text);
}

/* case-insensitive substring (strcasestr needs _GNU_SOURCE before
 * string.h, which this file can't guarantee) */
static int
ssid_match(const char *ssid, const char *needle)
{
	size_t nl = strlen(needle);

	if (!nl)
		return 1;
	for (; *ssid; ssid++)
		if (strncasecmp(ssid, needle, nl) == 0)
			return 1;
	return 0;
}

/* live filter: the search entry while typing, else the submitted one */
static const char *
net_filter(void)
{
	if (text_entry_active() &&
			strcmp(text_entry_label(), search_label) == 0)
		return text_entry_display();
	return ui_search;
}

static void
ask_psk(const char *ssid, int hidden)
{
	char label[96];

	snprintf(ui_pend_ssid, sizeof(ui_pend_ssid), "%s", ssid);
	ui_pend_hidden = hidden;
	snprintf(label, sizeof(label), "Password for %s", ssid);
	text_entry_begin(label, 1, psk_submitted, NULL);
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
	struct timespec now_ts;
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
	if (!popup_active)
		ui_search[0] = '\0';

	netmon_get(&s);
	memset(&ws, 0, sizeof(ws));
	if (s.wifi.present && !s.wifi_blocked && wifi_ctrl_ok()) {
		wifi_status_get(&ws);
		wifi_assoc = strcmp(ws.state, "COMPLETED") == 0;
	}
	ui_ws = ws;

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
		double elapsed = 0.0;

		request_public_ip_async_ex(1);
		netmon_gateway(ui_gateway, sizeof(ui_gateway));
		netmon_dns(ui_dns, sizeof(ui_dns));
		ui_stats_ok = netmon_stats(net_iface, &ui_stats) == 0;

		clock_gettime(CLOCK_MONOTONIC, &now_ts);
		if (net_prev_valid)
			elapsed = (now_ts.tv_sec - net_prev_ts.tv_sec)
				+ (now_ts.tv_nsec - net_prev_ts.tv_nsec) / 1e9;
		net_last_down_bps = (net_prev_valid && ui_stats_ok) ?
			net_bytes_to_rate(ui_stats.rx_bytes, net_prev_rx,
					elapsed) : -1.0;
		net_last_up_bps = (net_prev_valid && ui_stats_ok) ?
			net_bytes_to_rate(ui_stats.tx_bytes, net_prev_tx,
					elapsed) : -1.0;
		format_speed(net_last_down_bps, net_down_text,
				sizeof(net_down_text));
		format_speed(net_last_up_bps, net_up_text,
				sizeof(net_up_text));
		if (ui_stats_ok) {
			net_prev_rx = ui_stats.rx_bytes;
			net_prev_tx = ui_stats.tx_bytes;
			net_prev_ts = now_ts;
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

static const char *
sec_tag(const WifiNet *w)
{
	return w->secured ? "🔒" : "";
}

/* "a, b, c" -> "a · b +1", bounded so long resolver lists (or IPv6
 * servers) can never widen the card */
static void
dns_compact(const char *in, char *out, size_t len)
{
	char buf[128], *tok, *save;
	int n = 0, extra = 0;

	out[0] = '\0';
	snprintf(buf, sizeof(buf), "%s", in);
	for (tok = strtok_r(buf, ", ", &save); tok;
			tok = strtok_r(NULL, ", ", &save)) {
		if (n < 2 && strlen(out) + strlen(tok) < 36) {
			snprintf(out + strlen(out), len - strlen(out),
					"%s%s", n ? " · " : "", tok);
			n++;
		} else {
			extra++;
		}
	}
	if (extra)
		snprintf(out + strlen(out), len - strlen(out), " +%d", extra);
}

/* ── live spectrum overlay (wifi header) ─────────────────────────── */

#define SPEC_TICK_MS 33

static struct wlr_scene_buffer *spec_node;   /* child of view content;
                                              * reset on every re-render */
static int spec_x, spec_y, spec_w, spec_h;
static double spec_frac;
static const float *spec_accent;
static struct wl_event_source *spec_timer;

/* (Re)create the overlay node and swap in a freshly drawn frame. Safe
 * to call right after a card re-render so the spectrum never drops out
 * for a frame. */
static void
spec_overlay_draw(NetPopup *p)
{
	struct wlr_buffer *buf;

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
	buf = card_spectrum_buffer(spec_w, spec_h, spec_accent, spec_frac,
			monotonic_msec() / 1000.0);
	if (spec_node && buf)
		wlr_scene_buffer_set_buffer(spec_node, buf);
	if (buf)
		wlr_buffer_drop(buf);
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
		card_gap(card, 6);
		spec_frac = pct / 100.0;
		spec_accent = pct < 30 ? card_col_yellow : card_col_blue;
		card_wave(card, spec_frac, spec_accent);
		card_gap(card, 6);
	} else {
		card_header(card, net_icon_path, "Network", "DISCONNECTED",
				"--");
		card_gap(card, 8);
	}

	/* text entry (SSID / password prompt) */
	if (text_entry_active()) {
		card_section(card, "INPUT · ENTER = OK · ESC = CANCEL");
		card_text(card, text_entry_label(), NULL, NULL);
		card_text(card, text_entry_display(), NULL, card_col_blue);
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
		card_kv2(card, "Local IP",
				net_local_ip[0] ? net_local_ip : "--", NULL,
				"Gateway", ui_gateway[0] ? ui_gateway : "--",
				NULL);
		card_kv2(card, "Public IP",
				net_public_ip[0] ? net_public_ip : "--", NULL,
				NULL, NULL, NULL);
		if (ui_dns[0]) {
			/* text row, not kv2: kv2 width feeds the shared
			 * column grid, so one long value widens every row */
			dns_compact(ui_dns, v1, sizeof(v1));
			card_text(card, "DNS", v1, NULL);
		}

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
		card_kv2_btn(card, "Radio",
				s.wifi_blocked ? "Off" : "On",
				s.wifi_blocked ? NULL : card_col_green,
				"Toggle", s.wifi_blocked ? "Enable" : "Disable",
				NULL, NET_HIT_WIFI_TOGGLE,
				hot == NET_HIT_WIFI_TOGGLE);
	}
	if (s.wifi.present && !s.wifi_blocked) {
		const char *filter = net_filter();
		int folded = 0, listed = 0;

		if (wifi_assoc) {
			card_kv2(card, "SSID", ws.ssid, card_col_green,
					"BSSID", ws.bssid, NULL);
			snprintf(v1, sizeof(v1), "%d dBm · %.1f GHz",
					ws.signal_dbm, ws.freq_mhz / 1000.0);
			card_kv2(card, "Signal", v1, NULL, "Security",
					ws.key_mgmt[0] ? ws.key_mgmt : "Open",
					NULL);
			if (ws.link_speed_mbps > 0) {
				snprintf(v1, sizeof(v1), "%d Mbit/s",
						ws.link_speed_mbps);
				snprintf(v2, sizeof(v2), "%d Mbit/s",
						ws.max_rate_mbps);
				card_kv2(card, "Link rate", v1, card_col_green,
						"Max seen", v2, NULL);
			}
		}
		if (wifi_last_error()[0])
			card_text(card, wifi_last_error(), NULL, card_col_red);

		card_text_btn(card, "Networks",
				ui_scanning ? "Scanning…" : NULL,
				card_col_dim, "Scan", NET_HIT_SCAN,
				hot == NET_HIT_SCAN);
		card_text_btn(card, "Search",
				filter[0] ? filter : NULL,
				filter[0] ? card_col_blue : NULL,
				filter[0] ? "Clear" : "Find",
				NET_HIT_SEARCH, hot == NET_HIT_SEARCH);
		ui_nshown = ui_nnets < NET_LIST_MAX ? ui_nnets : NET_LIST_MAX;
		for (i = 0; i < ui_nshown; i++) {
			WifiNet *w = &ui_shown[i];
			const char *btn;

			*w = ui_nets[i];
			if (w->connected) {
				snprintf(v1, sizeof(v1), "%s %s",
						w->ssid, sec_tag(w));
				snprintf(v2, sizeof(v2), "Connected · %d%%",
						(int)lround(dbm_to_pct(
								w->signal_dbm)));
				card_text_btn(card, v1, v2, card_col_green,
						"Disconnect",
						NET_HIT_DISCONNECT,
						hot == NET_HIT_DISCONNECT);
				continue;
			}
			if (filter[0] && !ssid_match(w->ssid, filter))
				continue;
			if (!filter[0] && listed >= NET_LIST_FOLD) {
				folded++;
				continue;
			}
			listed++;
			snprintf(v1, sizeof(v1), "%s %s", w->ssid, sec_tag(w));
			snprintf(v2, sizeof(v2), "%d%% · %s%s",
					(int)lround(dbm_to_pct(w->signal_dbm)),
					w->sec[0] ? w->sec : "Open",
					w->known ? " · saved" : "");
			btn = w->known ? "Join" : (w->secured ? "Join…" : "Join");
			card_text_btn(card, v1, v2,
					w->known ? card_col_green : card_col_dim,
					btn, NET_HIT_NET_BASE + i,
					hot == NET_HIT_NET_BASE + i);
		}
		if (folded) {
			snprintf(v1, sizeof(v1), "+%d more · search to filter",
					folded);
			card_text(card, v1, NULL, NULL);
		}
		card_text_btn(card, "Hidden network", NULL, NULL, "Join…",
				NET_HIT_HIDDEN, hot == NET_HIT_HIDDEN);
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
			wifi_scan_request();
			ui_scanning = 1;
			ui_scan_req_ms = monotonic_msec();
		} else if (id == NET_HIT_SEARCH && button == BTN_LEFT) {
			if (text_entry_active() &&
					strcmp(text_entry_label(),
						search_label) == 0) {
				text_entry_cancel();
				ui_search[0] = '\0';
			} else if (ui_search[0]) {
				ui_search[0] = '\0';
			} else {
				text_entry_begin(search_label, 0,
						search_submitted, NULL);
			}
		} else if (id == NET_HIT_HIDDEN && button == BTN_LEFT) {
			text_entry_begin("Hidden network SSID", 0,
					hidden_ssid_submitted, NULL);
		} else if (id == NET_HIT_DISCONNECT && button == BTN_LEFT) {
			wifi_disconnect();
		} else if (id >= NET_HIT_NET_BASE &&
				id < NET_HIT_NET_BASE + NET_LIST_MAX) {
			WifiNet *w = &ui_shown[id - NET_HIT_NET_BASE];

			if (id - NET_HIT_NET_BASE >= ui_nshown) {
				/* stale list */
			} else if (button == BTN_RIGHT) {
				if (w->known)
					wifi_forget(w->known_id);
			} else if (w->known) {
				wifi_connect_known(w->known_id);
			} else if (w->secured) {
				ask_psk(w->ssid, 0);
			} else {
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
