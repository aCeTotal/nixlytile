/* NetworkManager coexistence backend.  While NM is running it owns the
 * wifi device and its wpa_supplicant instance (spawned over D-Bus with
 * no control socket), so the native wifi_ctrl path can neither scan nor
 * safely mutate supplicant state.  This backend serves the same wifi
 * API from a cache refreshed by nmcli via fetch_async — nothing here
 * ever blocks the compositor thread or fights NM for the device.
 * When NM is gone (the standalone future), wifi_ctrl takes over again.
 */
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "netsys.h"
#include "fetch_async.h"

#define NM_SAVED_MAX 32

static WifiNet nm_nets[WIFI_SCAN_MAX];
static int nm_nnets;
static WifiStatus nm_ws;
static int nm_max_rate;
static char nm_saved[NM_SAVED_MAX][64];
static int nm_nsaved;
static char nm_error[64];
static int nm_list_inflight, nm_saved_inflight;
static uint64_t nm_list_done_ms, nm_saved_done_ms;

static uint64_t
nm_now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* NM running?  /proc scan for a live "NetworkManager" comm, cached
 * 10s (no pidfile on NixOS, and /run/NetworkManager outlives a
 * stopped service).  NM's pid is low, so the scan exits early. */
int
nm_backend_active(void)
{
	static int cached;
	static uint64_t checked_ms;
	uint64_t now = nm_now_ms();
	struct dirent *de;
	DIR *d;

	if (checked_ms && now - checked_ms < 10000)
		return cached;
	checked_ms = now;
	cached = 0;
	d = opendir("/proc");
	if (!d)
		return 0;
	while ((de = readdir(d))) {
		char path[64], comm[32];
		FILE *f;

		if (de->d_name[0] < '0' || de->d_name[0] > '9')
			continue;
		snprintf(path, sizeof(path), "/proc/%s/comm", de->d_name);
		f = fopen(path, "r");
		if (!f)
			continue;
		if (fgets(comm, sizeof(comm), f) &&
				strncmp(comm, "NetworkManager", 14) == 0) {
			fclose(f);
			cached = 1;
			break;
		}
		fclose(f);
	}
	closedir(d);
	return cached;
}

/* Split one `nmcli -t` line in place; ':' inside a value arrives
 * escaped as "\:".  Returns the field count. */
static int
nm_split(char *line, char **f, int max)
{
	char *p = line, *w = line;
	int n = 0;

	f[n++] = w;
	while (*p) {
		if (*p == '\\' && p[1]) {
			*w++ = p[1];
			p += 2;
		} else if (*p == ':') {
			*w++ = '\0';
			p++;
			if (n >= max)
				break;
			f[n++] = w;
		} else {
			*w++ = *p++;
		}
	}
	*w = '\0';
	return n;
}

/* Profile name matches an SSID either exactly or as NM's duplicate
 * form "<ssid> <n>" ("Ragnarok 1" for SSID "Ragnarok"). */
static int
nm_name_matches(const char *name, const char *ssid)
{
	size_t sl = strlen(ssid);

	if (strncmp(name, ssid, sl) != 0)
		return 0;
	if (!name[sl])
		return 1;
	if (name[sl] != ' ')
		return 0;
	for (name += sl + 1; *name; name++)
		if (!isdigit((unsigned char)*name))
			return 0;
	return 1;
}

/* Single-quote s for sh -c, escaping embedded quotes. */
static void
nm_quote(char *dst, size_t len, const char *s)
{
	size_t o = 0;

	dst[o++] = '\'';
	for (; *s && o + 6 < len; s++) {
		if (*s == '\'') {
			memcpy(dst + o, "'\\''", 4);
			o += 4;
		} else {
			dst[o++] = *s;
		}
	}
	dst[o++] = '\'';
	dst[o] = '\0';
}

/* ── scan list + status (one nmcli call feeds both) ──────────────── */

static void
nm_list_done(const char *out, size_t len, void *data)
{
	static char buf[16384];
	WifiNet nets[WIFI_SCAN_MAX];
	WifiStatus ws;
	int count = 0, changed, i, j;
	char *line, *save;

	nm_list_inflight = 0;
	nm_list_done_ms = nm_now_ms();
	snprintf(buf, sizeof(buf), "%s", out);

	memset(&ws, 0, sizeof(ws));
	ws.active = 1;
	ws.signal_dbm = -127;
	ws.link_speed_mbps = -1;
	ws.rx_speed_mbps = -1;

	line = strtok_r(buf, "\n", &save);
	for (; line; line = strtok_r(NULL, "\n", &save)) {
		char *f[7];
		int active, sig_dbm, freq, rate, secured;

		if (nm_split(line, f, 7) < 7)
			continue;
		if (!f[1][0])
			continue;   /* hidden SSIDs join via the explicit dialog */
		active = strcmp(f[0], "yes") == 0;
		/* nmcli reports percent; the UI expects dBm (inverse of its
		 * own 2*(dbm+100) mapping) */
		sig_dbm = atoi(f[2]) / 2 - 100;
		freq = atoi(f[3]);
		rate = atoi(f[4]);
		secured = f[5][0] && strcmp(f[5], "--") != 0;

		if (active) {
			snprintf(ws.state, sizeof(ws.state), "COMPLETED");
			snprintf(ws.ssid, sizeof(ws.ssid), "%s", f[1]);
			snprintf(ws.bssid, sizeof(ws.bssid), "%s", f[6]);
			snprintf(ws.key_mgmt, sizeof(ws.key_mgmt), "%s", f[5]);
			ws.freq_mhz = freq;
			ws.signal_dbm = sig_dbm;
			ws.link_speed_mbps = rate;
			if (rate > nm_max_rate)
				nm_max_rate = rate;
		}

		/* keep only the strongest BSS per SSID */
		for (i = 0; i < count; i++)
			if (strcmp(nets[i].ssid, f[1]) == 0)
				break;
		if (i < count) {
			if (sig_dbm > nets[i].signal_dbm) {
				nets[i].signal_dbm = sig_dbm;
				snprintf(nets[i].bssid, sizeof(nets[i].bssid),
						"%s", f[6]);
				nets[i].freq_mhz = freq;
			}
			nets[i].connected |= active;
			continue;
		}
		if (count >= WIFI_SCAN_MAX)
			continue;
		{
			WifiNet *w = &nets[count++];

			memset(w, 0, sizeof(*w));
			snprintf(w->ssid, sizeof(w->ssid), "%s", f[1]);
			snprintf(w->bssid, sizeof(w->bssid), "%s", f[6]);
			w->freq_mhz = freq;
			w->signal_dbm = sig_dbm;
			w->secured = secured;
			w->connected = active;
			w->known_id = -1;
			for (j = 0; j < nm_nsaved; j++) {
				if (nm_name_matches(nm_saved[j], f[1])) {
					w->known = 1;
					w->known_id = j;
					break;
				}
			}
		}
	}
	ws.max_rate_mbps = nm_max_rate > 0 ? nm_max_rate : -1;

	/* strongest first */
	for (i = 0; i < count; i++)
		for (j = i + 1; j < count; j++)
			if (nets[j].signal_dbm > nets[i].signal_dbm) {
				WifiNet t = nets[i];
				nets[i] = nets[j];
				nets[j] = t;
			}

	changed = count != nm_nnets ||
		memcmp(nets, nm_nets, count * sizeof(nets[0])) != 0 ||
		memcmp(&ws, &nm_ws, sizeof(ws)) != 0;
	nm_nnets = count;
	memcpy(nm_nets, nets, sizeof(nm_nets));
	nm_ws = ws;
	if (changed)
		netsys_changed();
}

static void
nm_kick_list(uint64_t ttl_ms)
{
	uint64_t now = nm_now_ms();

	if (nm_list_inflight ||
			(nm_list_done_ms && now - nm_list_done_ms < ttl_ms))
		return;
	if (fetch_async("nmcli -t -f ACTIVE,SSID,SIGNAL,FREQ,RATE,SECURITY,BSSID "
				"dev wifi list --rescan no 2>/dev/null",
				nm_list_done, NULL) == 0)
		nm_list_inflight = 1;
}

/* ── saved profiles (known_id = index into nm_saved) ─────────────── */

static void
nm_saved_done(const char *out, size_t len, void *data)
{
	static char buf[8192];
	char *line, *save;

	nm_saved_inflight = 0;
	nm_saved_done_ms = nm_now_ms();
	snprintf(buf, sizeof(buf), "%s", out);
	nm_nsaved = 0;
	line = strtok_r(buf, "\n", &save);
	for (; line; line = strtok_r(NULL, "\n", &save)) {
		char *f[2];

		if (nm_split(line, f, 2) < 2)
			continue;
		if (strcmp(f[1], "802-11-wireless") != 0)
			continue;
		if (nm_nsaved >= NM_SAVED_MAX)
			break;
		snprintf(nm_saved[nm_nsaved], sizeof(nm_saved[0]), "%s", f[0]);
		nm_nsaved++;
	}
}

static void
nm_kick_saved(uint64_t ttl_ms)
{
	uint64_t now = nm_now_ms();

	if (nm_saved_inflight ||
			(nm_saved_done_ms && now - nm_saved_done_ms < ttl_ms))
		return;
	if (fetch_async("nmcli -t -f NAME,TYPE connection show 2>/dev/null",
				nm_saved_done, NULL) == 0)
		nm_saved_inflight = 1;
}

/* ── API ─────────────────────────────────────────────────────────── */

int
nm_wifi_status_get(WifiStatus *out)
{
	nm_kick_list(10000);
	*out = nm_ws;
	return 0;
}

int
nm_wifi_scan_get(WifiNet *out, int max)
{
	int n = nm_nnets < max ? nm_nnets : max;

	nm_kick_saved(15000);
	nm_kick_list(3000);
	memcpy(out, nm_nets, n * sizeof(out[0]));
	return n;
}

static void
nm_rescan_done(const char *out, size_t len, void *data)
{
	nm_list_done_ms = 0;   /* pick up fresh results on the next tick */
	netsys_changed();
}

void
nm_wifi_scan_request(void)
{
	fetch_async("nmcli dev wifi rescan 2>/dev/null", nm_rescan_done, NULL);
}

/* Actions report failure asynchronously: nmcli prints "Error: ..." on
 * the merged stream, which lands in nm_error for the popup. */
static void
nm_act_done(const char *out, size_t len, void *data)
{
	const char *err = strstr(out, "Error");

	if (err) {
		size_t n = strcspn(err, "\n");

		if (n >= sizeof(nm_error))
			n = sizeof(nm_error) - 1;
		memcpy(nm_error, err, n);
		nm_error[n] = '\0';
	} else {
		nm_error[0] = '\0';
	}
	nm_list_done_ms = 0;
	nm_saved_done_ms = 0;
	netsys_changed();
}

int
nm_wifi_connect(const char *ssid, const char *psk, int hidden)
{
	char qs[80], qp[200], cmd[384];

	nm_quote(qs, sizeof(qs), ssid);
	if (psk && psk[0]) {
		nm_quote(qp, sizeof(qp), psk);
		snprintf(cmd, sizeof(cmd),
				"nmcli dev wifi connect %s password %s%s 2>&1",
				qs, qp, hidden ? " hidden yes" : "");
	} else {
		snprintf(cmd, sizeof(cmd),
				"nmcli dev wifi connect %s%s 2>&1",
				qs, hidden ? " hidden yes" : "");
	}
	nm_error[0] = '\0';
	return fetch_async(cmd, nm_act_done, NULL);
}

int
nm_wifi_connect_known(int id)
{
	char qs[140], cmd[192];

	if (id < 0 || id >= nm_nsaved)
		return -1;
	nm_quote(qs, sizeof(qs), nm_saved[id]);
	snprintf(cmd, sizeof(cmd), "nmcli connection up id %s 2>&1", qs);
	nm_error[0] = '\0';
	return fetch_async(cmd, nm_act_done, NULL);
}

void
nm_wifi_disconnect(void)
{
	NetLinkSnap s;
	char cmd[128];

	netmon_get(&s);
	if (!s.wifi.present)
		return;
	snprintf(cmd, sizeof(cmd), "nmcli dev disconnect %s 2>&1",
			s.wifi.iface);
	fetch_async(cmd, nm_act_done, NULL);
}

int
nm_wifi_forget(int id)
{
	char qs[140], cmd[192];

	if (id < 0 || id >= nm_nsaved)
		return -1;
	nm_quote(qs, sizeof(qs), nm_saved[id]);
	snprintf(cmd, sizeof(cmd), "nmcli connection delete id %s 2>&1", qs);
	return fetch_async(cmd, nm_act_done, NULL);
}

const char *
nm_wifi_last_error(void)
{
	return nm_error;
}
