/* Native network + bluetooth subsystem (replaces NetworkManager/blueman).
 *
 * Kernel-direct where possible: rtnetlink + sysfs + ethtool ioctl for
 * links, /dev/rfkill for radio policy, wpa_supplicant's unix control
 * socket for wifi, sd-bus straight to bluetoothd for bluetooth, and
 * systemd units (via systemctl + polkit) for VPN profiles.  Everything
 * runs on the compositor event loop; no worker threads, no polling
 * while idle.
 */
#ifndef NETSYS_H
#define NETSYS_H

#include <net/if.h>
#include <stdint.h>

/* ── netmon.c: link state + stats (rtnetlink / sysfs / ethtool) ──── */

typedef struct {
	char iface[IF_NAMESIZE];
	int present;
	int carrier;            /* physical link up */
	int speed_mbps;         /* -1 unknown */
	int duplex_full;        /* -1 unknown, 0 half, 1 full */
	char mac[18];
} NetLink;

typedef struct {
	NetLink eth;
	NetLink wifi;
	int wifi_blocked;       /* rfkill soft-blocked */
	int wifi_user_on;       /* user forced wifi on while ethernet is up */
} NetLinkSnap;

typedef struct {
	unsigned long long rx_bytes, tx_bytes;
	unsigned long long rx_packets, tx_packets;
	unsigned long long rx_errors, tx_errors;
	unsigned long long rx_dropped, tx_dropped;
} NetIfStats;

void netmon_init(void);
void netmon_get(NetLinkSnap *out);
int netmon_stats(const char *iface, NetIfStats *out);   /* 0 on ok */
/* Route/DNS info for the popup (reads /proc/net/route + resolv.conf). */
int netmon_gateway(char *out, size_t len);
int netmon_dns(char *out, size_t len);
/* Radio policy: enable/disable wifi via rfkill.  user=1 marks it as an
 * explicit user choice that overrides the wifi-off-on-ethernet rule. */
void netmon_wifi_set_enabled(int on, int user);

/* ── wifi_ctrl.c: wpa_supplicant control interface ───────────────── */

#define WIFI_SCAN_MAX 24

typedef struct {
	char ssid[33];
	char bssid[18];
	int freq_mhz;
	int signal_dbm;
	int secured;            /* WPA/WPA2/WPA3/WEP */
	int known;              /* matches a saved network */
	int known_id;           /* wpa network id when known, else -1 */
	int connected;
} WifiNet;

typedef struct {
	int active;             /* ctrl connection alive */
	char state[24];         /* COMPLETED / SCANNING / ... */
	char ssid[33];
	char bssid[18];
	int freq_mhz;
	int signal_dbm;         /* from SIGNAL_POLL */
	int link_speed_mbps;    /* current tx rate */
	int rx_speed_mbps;
	int max_rate_mbps;      /* from scan entry of current bssid, if seen */
	char key_mgmt[32];
} WifiStatus;

void wifi_ctrl_init(void);
void wifi_ctrl_sync(void);          /* (re)connect ctrl socket if iface changed */
int wifi_ctrl_ok(void);
void wifi_scan_request(void);
int wifi_scan_get(WifiNet *out, int max);   /* returns count, strongest first */
int wifi_status_get(WifiStatus *out);
/* Connect to a network; psk NULL/"" for open networks.  hidden=1 sets
 * scan_ssid so hidden SSIDs are probed directly.  Saves config. */
int wifi_connect(const char *ssid, const char *psk, int hidden);
int wifi_connect_known(int net_id);
void wifi_disconnect(void);
int wifi_forget(int net_id);
/* Last auth failure ("wrong password"), cleared on connect; "" if none. */
const char *wifi_last_error(void);

/* ── wifi_nm.c: NetworkManager coexistence backend ───────────────────
 * While NM runs it owns the wifi device; the wifi_* API above delegates
 * here (nmcli via fetch_async, cache-backed) and netmon's rfkill policy
 * stands down so NM keeps working exactly as before. */

int nm_backend_active(void);
void nm_wifi_scan_request(void);
int nm_wifi_scan_get(WifiNet *out, int max);
int nm_wifi_status_get(WifiStatus *out);
int nm_wifi_connect(const char *ssid, const char *psk, int hidden);
int nm_wifi_connect_known(int id);
void nm_wifi_disconnect(void);
int nm_wifi_forget(int id);
const char *nm_wifi_last_error(void);

/* ── btmon.c: bluetoothd over sd-bus ─────────────────────────────── */

#define BT_DEV_MAX 24

typedef struct {
	char path[128];         /* D-Bus object path */
	char name[64];
	char addr[18];
	char icon[32];          /* bluez icon hint: audio-headset, input-mouse.. */
	int paired;
	int trusted;
	int connected;
	int rssi;               /* 0 = unknown */
	int battery;            /* -1 = unknown */
} BtDev;

typedef struct {
	int present;            /* adapter exists */
	int powered;
	int discovering;
	char name[64];
	char addr[18];
} BtAdapter;

void btmon_init(void);
int btmon_adapter(BtAdapter *out);
int btmon_devices(BtDev *out, int max);     /* returns count */
void btmon_set_powered(int on);
void btmon_set_discovering(int on);
void btmon_pair(const char *path);          /* pair + trust + connect */
void btmon_connect(const char *path);
void btmon_disconnect(const char *path);
void btmon_remove(const char *path);
/* OBEX file transfer: send a local file to a paired device. */
int btmon_send_file(const char *addr, const char *filepath);

/* ── vpnctl.c: VPN profiles as systemd units ─────────────────────── */

#define VPN_MAX 12

typedef struct {
	char unit[96];          /* e.g. wg-quick-wg0.service */
	char label[64];         /* display name, e.g. "wg0 (WireGuard)" */
	int active;
	int autoconnect;        /* unit enabled */
	int busy;               /* start/stop in flight */
} VpnProfile;

void vpnctl_init(void);
void vpnctl_refresh(void);                  /* async re-list units */
int vpnctl_profiles(VpnProfile *out, int max);
void vpnctl_toggle(int idx);                /* start/stop */
void vpnctl_set_autoconnect(int idx, int on);

/* ── text_entry.c: single-line input for popups ──────────────────── */

typedef void (*text_entry_submit_fn)(const char *text, void *data);

void text_entry_begin(const char *label, int masked,
		text_entry_submit_fn submit, void *data);
void text_entry_cancel(void);
int text_entry_active(void);
const char *text_entry_label(void);
/* Current content; masked entries return a run of dots for display. */
const char *text_entry_display(void);
/* Feed one xkb keysym (+ UTF-8 text, may be "") from the keyboard
 * handler.  Returns 1 when the key was consumed. */
int text_entry_key(uint32_t keysym, const char *utf8);

/* ── shared plumbing ─────────────────────────────────────────────── */

/* Poke the statusbar: something in net/bt/vpn state changed. */
void netsys_changed(void);
void btsys_changed(void);

/* bt_ui.c */
void refreshstatusbluetooth(void);

#endif
