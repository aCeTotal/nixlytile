/* wpa_supplicant control-interface client.  Two unix dgram sockets on
 * /run/wpa_supplicant/<iface>: one for synchronous commands (bounded by
 * a 300ms poll so a wedged supplicant can never stall the compositor),
 * one ATTACHed for unsolicited events and pumped by the event loop.
 * This covers everything wifi: scanning (incl. hidden SSIDs), connect,
 * saved networks, signal/linkspeed — no nl80211, no D-Bus, no nmcli.
 */
#include <ctype.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server-core.h>

#include "netsys.h"

extern struct wl_event_loop *event_loop;

static int wc_cmd_fd = -1;
static int wc_evt_fd = -1;
static struct wl_event_source *wc_evt_src;
static char wc_iface[IF_NAMESIZE];
static char wc_local_cmd[108];
static char wc_local_evt[108];
static char wc_error[64];
static int wc_max_linkspeed;
static int wc_pend_id = -1;   /* network added by wifi_connect, unsaved
                               * until CTRL-EVENT-CONNECTED confirms it */
static uint64_t wc_backoff_until;   /* wedged supplicant: skip requests */

static uint64_t
wc_now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void
wc_close(void)
{
	if (wc_evt_src) {
		wl_event_source_remove(wc_evt_src);
		wc_evt_src = NULL;
	}
	if (wc_cmd_fd >= 0) {
		close(wc_cmd_fd);
		wc_cmd_fd = -1;
	}
	if (wc_evt_fd >= 0) {
		close(wc_evt_fd);
		wc_evt_fd = -1;
	}
	if (wc_local_cmd[0])
		unlink(wc_local_cmd);
	if (wc_local_evt[0])
		unlink(wc_local_evt);
	wc_iface[0] = '\0';
}

static int
wc_open_sock(const char *iface, const char *tag, char *local, size_t llen)
{
	struct sockaddr_un sa;
	const char *rundir;
	int fd;

	fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;
	/* bind the reply socket under /run/wpa_supplicant/client: a
	 * sandboxed supplicant (RootDirectory=/run/wpa_supplicant) can't
	 * resolve XDG_RUNTIME_DIR paths, so replies sent there vanish */
	snprintf(local, llen, "/run/wpa_supplicant/client/nixlytile-%s-%d",
			tag, (int)getpid());
	unlink(local);
	memset(&sa, 0, sizeof(sa));
	sa.sun_family = AF_UNIX;
	snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", local);
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		rundir = getenv("XDG_RUNTIME_DIR");
		if (!rundir)
			rundir = "/tmp";
		snprintf(local, llen, "%s/nixlytile-wpa-%s-%d", rundir, tag,
				(int)getpid());
		unlink(local);
		memset(&sa, 0, sizeof(sa));
		sa.sun_family = AF_UNIX;
		snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", local);
		if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
			close(fd);
			return -1;
		}
	}
	/* the unprivileged supplicant must be able to write the reply */
	chmod(local, 0666);
	/* nixpkgs moved the ctrl sockets under a "control" subdir; try the
	 * new layout first, then the classic one. */
	memset(&sa, 0, sizeof(sa));
	sa.sun_family = AF_UNIX;
	snprintf(sa.sun_path, sizeof(sa.sun_path),
			"/run/wpa_supplicant/control/%s", iface);
	if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0)
		return fd;
	memset(&sa, 0, sizeof(sa));
	sa.sun_family = AF_UNIX;
	snprintf(sa.sun_path, sizeof(sa.sun_path),
			"/run/wpa_supplicant/%s", iface);
	if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		close(fd);
		unlink(local);
		return -1;
	}
	return fd;
}

/* Send a command, wait for the reply under a hard 300ms deadline (this
 * runs on the compositor thread — a wedged supplicant must never freeze
 * input).  A timeout arms a 3s backoff so hover-driven refreshes don't
 * keep re-stalling.  Unsolicited event lines (prefixed '<') that land
 * on the command socket are skipped within the same deadline.
 * Returns reply length, or -1. */
static int
wc_request(const char *cmd, char *reply, size_t rlen)
{
	struct pollfd pfd;
	uint64_t deadline;

	if (wc_cmd_fd < 0)
		return -1;
	if (wc_backoff_until && wc_now_ms() < wc_backoff_until)
		return -1;
	if (send(wc_cmd_fd, cmd, strlen(cmd), 0) < 0)
		return -1;
	pfd.fd = wc_cmd_fd;
	pfd.events = POLLIN;
	deadline = wc_now_ms() + 300;
	for (;;) {
		uint64_t now = wc_now_ms();
		ssize_t n;

		if (now >= deadline)
			break;
		if (poll(&pfd, 1, (int)(deadline - now)) <= 0)
			break;
		n = recv(wc_cmd_fd, reply, rlen - 1, 0);
		if (n < 0)
			break;
		reply[n] = '\0';
		if (reply[0] != '<') {
			wc_backoff_until = 0;
			return (int)n;
		}
	}
	wc_backoff_until = wc_now_ms() + 3000;
	return -1;
}

static int
wc_cmd_ok(const char *cmd)
{
	char reply[64];

	if (wc_request(cmd, reply, sizeof(reply)) < 0)
		return -1;
	return strncmp(reply, "OK", 2) == 0 ? 0 : -1;
}

static int
wc_evt_event(int fd, uint32_t mask, void *data)
{
	char buf[512];
	ssize_t n;
	int poke = 0;

	if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
		wc_close();
		netsys_changed();
		return 0;
	}
	while ((n = recv(fd, buf, sizeof(buf) - 1, MSG_DONTWAIT)) > 0) {
		buf[n] = '\0';
		if (strstr(buf, "CTRL-EVENT-SCAN-RESULTS") ||
				strstr(buf, "CTRL-EVENT-CONNECTED") ||
				strstr(buf, "CTRL-EVENT-DISCONNECTED"))
			poke = 1;
		if (strstr(buf, "CTRL-EVENT-CONNECTED")) {
			wc_error[0] = '\0';
			/* SELECT_NETWORK disabled every other saved network
			 * so the chosen one couldn't be outraced; now that the
			 * association stuck, restore them for future roaming */
			wc_cmd_ok("ENABLE_NETWORK all");
			/* persist the new network only now that it worked, so
			 * it auto-connects from here on */
			if (wc_pend_id >= 0) {
				wc_cmd_ok("SAVE_CONFIG");
				wc_pend_id = -1;
			}
		}
		if (strstr(buf, "CTRL-EVENT-SSID-TEMP-DISABLED") &&
				strstr(buf, "WRONG_KEY")) {
			snprintf(wc_error, sizeof(wc_error), "Wrong password");
			/* drop the failed attempt so a bad password is never
			 * saved and never retried */
			if (wc_pend_id >= 0) {
				char cmd[64];

				snprintf(cmd, sizeof(cmd), "REMOVE_NETWORK %d",
						wc_pend_id);
				wc_cmd_ok(cmd);
				wc_pend_id = -1;
			}
			poke = 1;
		}
		if (strstr(buf, "CTRL-EVENT-AUTH-REJECT")) {
			snprintf(wc_error, sizeof(wc_error), "Auth rejected");
			poke = 1;
		}
	}
	if (poke)
		netsys_changed();
	return 0;
}

void
wifi_ctrl_init(void)
{
	wifi_ctrl_sync();
}

/* (Re)connect the ctrl sockets when the wifi interface appears,
 * disappears or changes name.  Called from netmon on every rescan. */
void
wifi_ctrl_sync(void)
{
	NetLinkSnap s;

	if (nm_backend_active()) {
		/* NM owns the supplicant; never attach to (or mutate) it */
		if (wc_cmd_fd >= 0)
			wc_close();
		return;
	}
	netmon_get(&s);
	if (!s.wifi.present || s.wifi_blocked) {
		if (wc_cmd_fd >= 0)
			wc_close();
		return;
	}
	if (wc_cmd_fd >= 0 && strcmp(wc_iface, s.wifi.iface) == 0)
		return;
	wc_close();
	wc_cmd_fd = wc_open_sock(s.wifi.iface, "cmd",
			wc_local_cmd, sizeof(wc_local_cmd));
	if (wc_cmd_fd < 0)
		return;
	wc_evt_fd = wc_open_sock(s.wifi.iface, "evt",
			wc_local_evt, sizeof(wc_local_evt));
	if (wc_evt_fd >= 0) {
		char reply[16];
		ssize_t n;
		struct pollfd pfd = { .fd = wc_evt_fd, .events = POLLIN };

		if (send(wc_evt_fd, "ATTACH", 6, 0) >= 0 &&
				poll(&pfd, 1, 300) > 0 &&
				(n = recv(wc_evt_fd, reply, sizeof(reply) - 1, 0)) > 0) {
			reply[n] = '\0';
			if (strncmp(reply, "OK", 2) == 0)
				wc_evt_src = wl_event_loop_add_fd(event_loop,
						wc_evt_fd, WL_EVENT_READABLE,
						wc_evt_event, NULL);
		}
		if (!wc_evt_src) {
			close(wc_evt_fd);
			wc_evt_fd = -1;
			unlink(wc_local_evt);
		}
	}
	snprintf(wc_iface, sizeof(wc_iface), "%s", s.wifi.iface);
}

int
wifi_ctrl_ok(void)
{
	if (nm_backend_active())
		return 1;
	return wc_cmd_fd >= 0;
}

void
wifi_scan_request(void)
{
	if (nm_backend_active()) {
		nm_wifi_scan_request();
		return;
	}
	wc_cmd_ok("SCAN");
}

/* Human label for the scan flags column, e.g. "[WPA2-SAE-CCMP][ESS]". */
static void
wc_sec_label(const char *flags, char *out, size_t len)
{
	int sae = strstr(flags, "SAE") != NULL;
	int psk = strstr(flags, "PSK") != NULL;

	if (sae && psk)
		snprintf(out, len, "WPA2/3");
	else if (sae)
		snprintf(out, len, "WPA3");
	else if (strstr(flags, "WPA2"))
		snprintf(out, len, "WPA2");
	else if (strstr(flags, "WPA"))
		snprintf(out, len, "WPA");
	else if (strstr(flags, "WEP"))
		snprintf(out, len, "WEP");
	else
		snprintf(out, len, "Open");
}

/* Saved networks: id -> ssid map used to mark scan entries as known. */
struct saved_net {
	int id;
	char ssid[33];
	int current;
};

static int
wifi_list_saved(struct saved_net *out, int max)
{
	static char buf[8192];
	char *line, *save;
	int count = 0;

	if (wc_request("LIST_NETWORKS", buf, sizeof(buf)) < 0)
		return 0;
	line = strtok_r(buf, "\n", &save);   /* header */
	while (line && count < max) {
		line = strtok_r(NULL, "\n", &save);
		if (!line)
			break;
		{
			struct saved_net *sn = &out[count];
			char *tab1 = strchr(line, '\t');
			char *tab2 = tab1 ? strchr(tab1 + 1, '\t') : NULL;

			if (!tab1 || !tab2)
				continue;
			sn->id = atoi(line);
			*tab2 = '\0';
			snprintf(sn->ssid, sizeof(sn->ssid), "%s", tab1 + 1);
			sn->current = strstr(tab2 + 1, "[CURRENT]") != NULL;
			count++;
		}
	}
	return count;
}

int
wifi_scan_get(WifiNet *out, int max)
{
	static char buf[16384];
	struct saved_net saved[32];
	int nsaved, count = 0;
	char *line, *save;

	if (nm_backend_active())
		return nm_wifi_scan_get(out, max);
	if (wc_request("SCAN_RESULTS", buf, sizeof(buf)) < 0)
		return 0;
	nsaved = wifi_list_saved(saved, 32);

	line = strtok_r(buf, "\n", &save);   /* header */
	while (count < max) {
		char bssid[18], flags[128], ssid[128];
		int freq, sig, i;

		line = strtok_r(NULL, "\n", &save);
		if (!line)
			break;
		ssid[0] = flags[0] = '\0';
		if (sscanf(line, "%17s %d %d %127s %127[^\n]",
					bssid, &freq, &sig, flags, ssid) < 4)
			continue;
		if (!ssid[0])
			continue;   /* hidden SSIDs join via the explicit dialog */
		/* keep only the strongest BSS per SSID */
		for (i = 0; i < count; i++)
			if (strcmp(out[i].ssid, ssid) == 0)
				break;
		if (i < count) {
			if (sig > out[i].signal_dbm) {
				out[i].signal_dbm = sig;
				snprintf(out[i].bssid, sizeof(out[i].bssid),
						"%s", bssid);
				out[i].freq_mhz = freq;
			}
			continue;
		}
		{
			WifiNet *w = &out[count++];

			memset(w, 0, sizeof(*w));
			snprintf(w->ssid, sizeof(w->ssid), "%s", ssid);
			snprintf(w->bssid, sizeof(w->bssid), "%s", bssid);
			w->freq_mhz = freq;
			w->signal_dbm = sig;
			w->secured = strstr(flags, "WPA") != NULL ||
				strstr(flags, "WEP") != NULL ||
				strstr(flags, "SAE") != NULL;
			wc_sec_label(flags, w->sec, sizeof(w->sec));
			w->known_id = -1;
			for (i = 0; i < nsaved; i++) {
				if (strcmp(saved[i].ssid, ssid) == 0) {
					w->known = 1;
					w->known_id = saved[i].id;
					w->connected = saved[i].current;
					break;
				}
			}
		}
	}
	/* strongest first */
	{
		int i, j;

		for (i = 0; i < count; i++)
			for (j = i + 1; j < count; j++)
				if (out[j].signal_dbm > out[i].signal_dbm) {
					WifiNet t = out[i];
					out[i] = out[j];
					out[j] = t;
				}
	}
	return count;
}

static void
wc_kv(const char *buf, const char *key, char *out, size_t len)
{
	const char *p = buf;
	size_t klen = strlen(key);

	out[0] = '\0';
	while (p) {
		if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
			const char *e = strchr(p, '\n');
			size_t n = e ? (size_t)(e - p - klen - 1) : strlen(p + klen + 1);

			if (n >= len)
				n = len - 1;
			memcpy(out, p + klen + 1, n);
			out[n] = '\0';
			return;
		}
		p = strchr(p, '\n');
		if (p)
			p++;
	}
}

int
wifi_status_get(WifiStatus *out)
{
	static char buf[4096];
	char val[64];

	if (nm_backend_active())
		return nm_wifi_status_get(out);
	memset(out, 0, sizeof(*out));
	out->signal_dbm = -127;
	out->link_speed_mbps = -1;
	out->rx_speed_mbps = -1;
	out->max_rate_mbps = wc_max_linkspeed > 0 ? wc_max_linkspeed : -1;
	if (wc_cmd_fd < 0)
		return -1;
	if (wc_request("STATUS", buf, sizeof(buf)) < 0)
		return -1;
	out->active = 1;
	wc_kv(buf, "wpa_state", out->state, sizeof(out->state));
	wc_kv(buf, "ssid", out->ssid, sizeof(out->ssid));
	wc_kv(buf, "bssid", out->bssid, sizeof(out->bssid));
	wc_kv(buf, "key_mgmt", out->key_mgmt, sizeof(out->key_mgmt));
	wc_kv(buf, "freq", val, sizeof(val));
	out->freq_mhz = atoi(val);

	if (strcmp(out->state, "COMPLETED") == 0 &&
			wc_request("SIGNAL_POLL", buf, sizeof(buf)) >= 0) {
		wc_kv(buf, "RSSI", val, sizeof(val));
		if (val[0])
			out->signal_dbm = atoi(val);
		wc_kv(buf, "LINKSPEED", val, sizeof(val));
		if (val[0]) {
			out->link_speed_mbps = atoi(val);
			if (out->link_speed_mbps > wc_max_linkspeed)
				wc_max_linkspeed = out->link_speed_mbps;
			out->max_rate_mbps = wc_max_linkspeed;
		}
		wc_kv(buf, "FREQUENCY", val, sizeof(val));
		if (val[0])
			out->freq_mhz = atoi(val);
	}
	return 0;
}

int
wifi_connect(const char *ssid, const char *psk, int hidden)
{
	char reply[64], cmd[256];
	int id;

	if (nm_backend_active())
		return nm_wifi_connect(ssid, psk, hidden);
	if (wc_request("ADD_NETWORK", reply, sizeof(reply)) < 0)
		return -1;
	if (!isdigit((unsigned char)reply[0]))
		return -1;   /* "FAIL" — never touch network id 0 by accident */
	id = atoi(reply);
	snprintf(cmd, sizeof(cmd), "SET_NETWORK %d ssid \"%s\"", id, ssid);
	if (wc_cmd_ok(cmd) != 0)
		goto fail;
	if (psk && psk[0]) {
		snprintf(cmd, sizeof(cmd),
				"SET_NETWORK %d psk \"%s\"", id, psk);
		if (wc_cmd_ok(cmd) != 0)
			goto fail;
		/* allow WPA2 and WPA3-personal; older supplicants reject
		 * SAE here, which is fine — default already covers PSK */
		snprintf(cmd, sizeof(cmd),
				"SET_NETWORK %d key_mgmt WPA-PSK SAE", id);
		if (wc_cmd_ok(cmd) == 0) {
			snprintf(cmd, sizeof(cmd),
					"SET_NETWORK %d ieee80211w 1", id);
			wc_cmd_ok(cmd);
		}
	} else {
		snprintf(cmd, sizeof(cmd),
				"SET_NETWORK %d key_mgmt NONE", id);
		if (wc_cmd_ok(cmd) != 0)
			goto fail;
	}
	if (hidden) {
		snprintf(cmd, sizeof(cmd),
				"SET_NETWORK %d scan_ssid 1", id);
		wc_cmd_ok(cmd);
	}
	wc_error[0] = '\0';
	snprintf(cmd, sizeof(cmd), "SELECT_NETWORK %d", id);
	if (wc_cmd_ok(cmd) != 0)
		goto fail;
	/* an earlier attempt that never connected is dead weight */
	if (wc_pend_id >= 0 && wc_pend_id != id) {
		snprintf(cmd, sizeof(cmd), "REMOVE_NETWORK %d", wc_pend_id);
		wc_cmd_ok(cmd);
	}
	wc_pend_id = id;   /* saved on CTRL-EVENT-CONNECTED */
	return 0;
fail:
	snprintf(cmd, sizeof(cmd), "REMOVE_NETWORK %d", id);
	wc_cmd_ok(cmd);
	return -1;
}

int
wifi_connect_known(int net_id)
{
	char cmd[64];

	if (nm_backend_active())
		return nm_wifi_connect_known(net_id);
	wc_error[0] = '\0';
	/* abandon any unconfirmed attempt so CONNECTED can't save it */
	if (wc_pend_id >= 0 && wc_pend_id != net_id) {
		snprintf(cmd, sizeof(cmd), "REMOVE_NETWORK %d", wc_pend_id);
		wc_cmd_ok(cmd);
		wc_pend_id = -1;
	}
	snprintf(cmd, sizeof(cmd), "SELECT_NETWORK %d", net_id);
	if (wc_cmd_ok(cmd) != 0)
		return -1;
	return 0;
}

void
wifi_disconnect(void)
{
	if (nm_backend_active()) {
		nm_wifi_disconnect();
		return;
	}
	wc_cmd_ok("DISCONNECT");
}

int
wifi_forget(int net_id)
{
	char cmd[64];

	if (nm_backend_active())
		return nm_wifi_forget(net_id);
	snprintf(cmd, sizeof(cmd), "REMOVE_NETWORK %d", net_id);
	if (wc_cmd_ok(cmd) != 0)
		return -1;
	wc_cmd_ok("SAVE_CONFIG");
	return 0;
}

const char *
wifi_last_error(void)
{
	if (nm_backend_active())
		return nm_wifi_last_error();
	return wc_error;
}
