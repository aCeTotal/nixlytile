/* Bluetooth: talks straight to bluetoothd on the system bus (the only
 * daemon the kernel stack needs for pairing/profiles) — no blueman.
 * Mirrors adapter + devices from ObjectManager, registers a pairing
 * agent (passkeys are shown on the OSD and confirmed automatically),
 * and an OBEX agent on the session bus so incoming file transfers land
 * in ~/Downloads.  Bus fds are pumped by the compositor event loop
 * exactly like tray.c does; on bus HUP the source is removed (never
 * spin on a dead fd).
 */
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <systemd/sd-bus.h>
#include <unistd.h>
#include <wayland-server-core.h>

#include "netsys.h"

extern struct wl_event_loop *event_loop;
typedef struct Monitor Monitor;
extern Monitor *selmon;
void osd_show_force(Monitor *m, const char *msg);

static sd_bus *bt_bus;                  /* system bus: org.bluez */
static sd_bus *obex_bus;                /* session bus: org.bluez.obex */
static struct wl_event_source *bt_src;
static struct wl_event_source *obex_src;
static struct wl_event_source *bt_timer;
static struct wl_event_source *obex_timer;

static BtAdapter bt_adapter;
static char bt_adapter_path[128];
static BtDev bt_devs[BT_DEV_MAX];
static int bt_ndevs;

/* ── model ───────────────────────────────────────────────────────── */

static BtDev *
dev_find(const char *path)
{
	int i;

	for (i = 0; i < bt_ndevs; i++)
		if (strcmp(bt_devs[i].path, path) == 0)
			return &bt_devs[i];
	return NULL;
}

static BtDev *
dev_add(const char *path)
{
	BtDev *d = dev_find(path);

	if (d)
		return d;
	if (bt_ndevs >= BT_DEV_MAX)
		return NULL;
	d = &bt_devs[bt_ndevs++];
	memset(d, 0, sizeof(*d));
	snprintf(d->path, sizeof(d->path), "%s", path);
	d->battery = -1;
	return d;
}

static void
dev_remove(const char *path)
{
	int i;

	for (i = 0; i < bt_ndevs; i++) {
		if (strcmp(bt_devs[i].path, path) == 0) {
			memmove(&bt_devs[i], &bt_devs[i + 1],
					(bt_ndevs - i - 1) * sizeof(BtDev));
			bt_ndevs--;
			return;
		}
	}
}

/* ── property parsing ────────────────────────────────────────────── */

/* Typed variant readers: on a signature mismatch the variant is skipped
 * instead of leaving the message mis-positioned (which would abort the
 * whole ObjectManager dump on one odd property). */
static int
var_str(sd_bus_message *m, char *dst, size_t len)
{
	const char *s;

	if (sd_bus_message_read(m, "v", "s", &s) >= 0) {
		snprintf(dst, len, "%s", s);
		return 0;
	}
	sd_bus_message_skip(m, "v");
	return -1;
}

static int
var_bool(sd_bus_message *m, int *dst)
{
	int b;

	if (sd_bus_message_read(m, "v", "b", &b) >= 0) {
		*dst = b;
		return 0;
	}
	sd_bus_message_skip(m, "v");
	return -1;
}

static int
var_i16(sd_bus_message *m, int *dst)
{
	int16_t v;

	if (sd_bus_message_read(m, "v", "n", &v) >= 0) {
		*dst = v;
		return 0;
	}
	sd_bus_message_skip(m, "v");
	return -1;
}

static int
var_u8(sd_bus_message *m, int *dst)
{
	uint8_t v;

	if (sd_bus_message_read(m, "v", "y", &v) >= 0) {
		*dst = v;
		return 0;
	}
	sd_bus_message_skip(m, "v");
	return -1;
}

/* Parse one a{sv} dict of org.bluez.Device1 / Adapter1 / Battery1
 * properties into the model.  msg positioned at the a{sv} container. */
static int
parse_props(sd_bus_message *m, const char *iface, const char *path)
{
	int r;

	r = sd_bus_message_enter_container(m, 'a', "{sv}");
	if (r < 0)
		return r;
	while ((r = sd_bus_message_enter_container(m, 'e', "sv")) > 0) {
		const char *key;

		r = sd_bus_message_read(m, "s", &key);
		if (r < 0)
			return r;
		if (strcmp(iface, "org.bluez.Adapter1") == 0 &&
				strcmp(path, bt_adapter_path) == 0) {
			if (strcmp(key, "Powered") == 0)
				var_bool(m, &bt_adapter.powered);
			else if (strcmp(key, "Discovering") == 0)
				var_bool(m, &bt_adapter.discovering);
			else if (strcmp(key, "Name") == 0)
				var_str(m, bt_adapter.name,
						sizeof(bt_adapter.name));
			else if (strcmp(key, "Address") == 0)
				var_str(m, bt_adapter.addr,
						sizeof(bt_adapter.addr));
			else
				sd_bus_message_skip(m, "v");
		} else if (strcmp(iface, "org.bluez.Device1") == 0) {
			BtDev *d = dev_add(path);

			if (!d)
				sd_bus_message_skip(m, "v");
			else if (strcmp(key, "Alias") == 0)
				var_str(m, d->name, sizeof(d->name));
			else if (strcmp(key, "Address") == 0)
				var_str(m, d->addr, sizeof(d->addr));
			else if (strcmp(key, "Icon") == 0)
				var_str(m, d->icon, sizeof(d->icon));
			else if (strcmp(key, "Paired") == 0)
				var_bool(m, &d->paired);
			else if (strcmp(key, "Trusted") == 0)
				var_bool(m, &d->trusted);
			else if (strcmp(key, "Connected") == 0)
				var_bool(m, &d->connected);
			else if (strcmp(key, "RSSI") == 0)
				var_i16(m, &d->rssi);
			else
				sd_bus_message_skip(m, "v");
		} else if (strcmp(iface, "org.bluez.Battery1") == 0) {
			BtDev *d = dev_find(path);

			if (d && strcmp(key, "Percentage") == 0)
				var_u8(m, &d->battery);
			else
				sd_bus_message_skip(m, "v");
		} else {
			sd_bus_message_skip(m, "v");
		}
		r = sd_bus_message_exit_container(m);
		if (r < 0)
			return r;
	}
	return sd_bus_message_exit_container(m);
}

/* Parse a{sa{sv}} (interfaces + props) for one object path. */
static int
parse_object_ifaces(sd_bus_message *m, const char *path)
{
	int r;

	r = sd_bus_message_enter_container(m, 'a', "{sa{sv}}");
	if (r < 0)
		return r;
	while ((r = sd_bus_message_enter_container(m, 'e', "sa{sv}")) > 0) {
		const char *iface;

		r = sd_bus_message_read(m, "s", &iface);
		if (r < 0)
			return r;
		if (strcmp(iface, "org.bluez.Adapter1") == 0 &&
				!bt_adapter_path[0]) {
			bt_adapter.present = 1;
			snprintf(bt_adapter_path, sizeof(bt_adapter_path),
					"%s", path);
		}
		r = parse_props(m, iface, path);
		if (r < 0)
			return r;
		r = sd_bus_message_exit_container(m);
		if (r < 0)
			return r;
	}
	return sd_bus_message_exit_container(m);
}

static int
managed_objects_cb(sd_bus_message *m, void *userdata, sd_bus_error *err)
{
	int r;

	if (sd_bus_message_is_method_error(m, NULL))
		return 0;
	r = sd_bus_message_enter_container(m, 'a', "{oa{sa{sv}}}");
	if (r < 0)
		return 0;
	while ((r = sd_bus_message_enter_container(m, 'e', "oa{sa{sv}}")) > 0) {
		const char *path;

		if (sd_bus_message_read(m, "o", &path) < 0)
			break;
		parse_object_ifaces(m, path);
		sd_bus_message_exit_container(m);
	}
	btsys_changed();
	return 0;
}

/* ── signal handlers ─────────────────────────────────────────────── */

static int
interfaces_added_cb(sd_bus_message *m, void *userdata, sd_bus_error *err)
{
	const char *path;

	if (sd_bus_message_read(m, "o", &path) < 0)
		return 0;
	parse_object_ifaces(m, path);
	btsys_changed();
	return 0;
}

static int
interfaces_removed_cb(sd_bus_message *m, void *userdata, sd_bus_error *err)
{
	const char *path, *iface;

	if (sd_bus_message_read(m, "o", &path) < 0)
		return 0;
	if (sd_bus_message_enter_container(m, 'a', "s") < 0)
		return 0;
	while (sd_bus_message_read(m, "s", &iface) > 0) {
		if (strcmp(iface, "org.bluez.Device1") == 0)
			dev_remove(path);
		else if (strcmp(iface, "org.bluez.Adapter1") == 0 &&
				strcmp(path, bt_adapter_path) == 0) {
			memset(&bt_adapter, 0, sizeof(bt_adapter));
			bt_adapter_path[0] = '\0';
			bt_ndevs = 0;
		}
	}
	btsys_changed();
	return 0;
}

static int
props_changed_cb(sd_bus_message *m, void *userdata, sd_bus_error *err)
{
	const char *iface;
	const char *path = sd_bus_message_get_path(m);

	if (!path || sd_bus_message_read(m, "s", &iface) < 0)
		return 0;
	if (strncmp(iface, "org.bluez.", 10) != 0)
		return 0;
	parse_props(m, iface, path);
	btsys_changed();
	return 0;
}

/* ── pairing agent ───────────────────────────────────────────────── */

static void
bt_osd(const char *msg)
{
	if (selmon)
		osd_show_force(selmon, msg);
}

static int
agent_request_confirmation(sd_bus_message *m, void *ud, sd_bus_error *err)
{
	const char *dev;
	uint32_t passkey = 0;
	char buf[96];

	sd_bus_message_read(m, "ou", &dev, &passkey);
	snprintf(buf, sizeof(buf), "Bluetooth pairing: %06u", passkey);
	bt_osd(buf);
	return sd_bus_reply_method_return(m, "");
}

static int
agent_display_passkey(sd_bus_message *m, void *ud, sd_bus_error *err)
{
	const char *dev;
	uint32_t passkey = 0;
	char buf[96];

	sd_bus_message_read(m, "ou", &dev, &passkey);
	snprintf(buf, sizeof(buf), "Bluetooth passkey: %06u", passkey);
	bt_osd(buf);
	return sd_bus_reply_method_return(m, "");
}

static int
agent_authorize(sd_bus_message *m, void *ud, sd_bus_error *err)
{
	return sd_bus_reply_method_return(m, "");
}

/* Legacy BR/EDR pairing (old headsets, car kits): supply the standard
 * fixed PIN and show it so the user can type it on devices that ask. */
static int
agent_request_pincode(sd_bus_message *m, void *ud, sd_bus_error *err)
{
	bt_osd("Bluetooth PIN: 0000");
	return sd_bus_reply_method_return(m, "s", "0000");
}

static int
agent_request_passkey(sd_bus_message *m, void *ud, sd_bus_error *err)
{
	bt_osd("Bluetooth passkey: 000000");
	return sd_bus_reply_method_return(m, "u", (uint32_t)0);
}

static int
agent_cancel(sd_bus_message *m, void *ud, sd_bus_error *err)
{
	bt_osd("Bluetooth pairing cancelled");
	return sd_bus_reply_method_return(m, "");
}

static const sd_bus_vtable agent_vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD("RequestConfirmation", "ou", "",
			agent_request_confirmation, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("DisplayPasskey", "ouq", "",
			agent_display_passkey, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("DisplayPinCode", "os", "",
			agent_authorize, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("RequestPinCode", "o", "s",
			agent_request_pincode, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("RequestPasskey", "o", "u",
			agent_request_passkey, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("RequestAuthorization", "o", "",
			agent_authorize, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("AuthorizeService", "os", "",
			agent_authorize, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("Cancel", "", "", agent_cancel,
			SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("Release", "", "", agent_authorize,
			SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_VTABLE_END
};

#define BT_AGENT_PATH "/org/nixlytile/btagent"

/* ── OBEX receive agent (session bus) ────────────────────────────── */

static int
obex_authorize_push(sd_bus_message *m, void *ud, sd_bus_error *err)
{
	const char *transfer;
	static char dest[512];
	static unsigned seq;
	const char *home = getenv("HOME");

	sd_bus_message_read(m, "o", &transfer);
	snprintf(dest, sizeof(dest), "%s/Downloads/bt-incoming-%u",
			home ? home : "/tmp", ++seq);
	bt_osd("Bluetooth: receiving file");
	return sd_bus_reply_method_return(m, "s", dest);
}

static int
obex_agent_noop(sd_bus_message *m, void *ud, sd_bus_error *err)
{
	return sd_bus_reply_method_return(m, "");
}

static const sd_bus_vtable obex_agent_vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD("AuthorizePush", "o", "s",
			obex_authorize_push, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("Cancel", "", "", obex_agent_noop,
			SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("Release", "", "", obex_agent_noop,
			SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_VTABLE_END
};

#define OBEX_AGENT_PATH "/org/nixlytile/obexagent"

/* ── bus pumping (tray.c recipe) ─────────────────────────────────── */

static void
bus_update_mask(sd_bus *bus, struct wl_event_source *src)
{
	int ev = sd_bus_get_events(bus);
	uint32_t mask = 0;

	if (ev >= 0) {
		if (ev & POLLIN)
			mask |= WL_EVENT_READABLE;
		if (ev & POLLOUT)
			mask |= WL_EVENT_WRITABLE;
	}
	if (!mask)
		mask = WL_EVENT_READABLE;
	wl_event_source_fd_update(src, mask);
}

/* sd-bus method-call timeouts fire from sd_bus_process, which only runs
 * when the fd wakes; an idle bus would never expire a lost call and its
 * callback (and userdata) would leak.  Mirror sd_bus_get_timeout into a
 * wayland timer. */
static void
bus_arm_timer(sd_bus *bus, struct wl_event_source *timer)
{
	uint64_t usec = UINT64_MAX;
	struct timespec ts;
	uint64_t now_us;

	if (!bus || !timer)
		return;
	if (sd_bus_get_timeout(bus, &usec) < 0 || usec == UINT64_MAX) {
		wl_event_source_timer_update(timer, 0);
		return;
	}
	clock_gettime(CLOCK_MONOTONIC, &ts);
	now_us = (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
	if (usec <= now_us)
		wl_event_source_timer_update(timer, 1);
	else
		wl_event_source_timer_update(timer,
				(int)((usec - now_us) / 1000 + 1));
}

static int
bt_timer_cb(void *data)
{
	if (!bt_bus)
		return 0;
	while (sd_bus_process(bt_bus, NULL) > 0)
		;
	if (bt_bus && bt_src)
		bus_update_mask(bt_bus, bt_src);
	bus_arm_timer(bt_bus, bt_timer);
	return 0;
}

static int
obex_timer_cb(void *data)
{
	if (!obex_bus)
		return 0;
	while (sd_bus_process(obex_bus, NULL) > 0)
		;
	if (obex_bus && obex_src)
		bus_update_mask(obex_bus, obex_src);
	bus_arm_timer(obex_bus, obex_timer);
	return 0;
}

static int
bt_bus_event(int fd, uint32_t mask, void *data)
{
	if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
		/* dead bus: remove the source or epoll spins forever */
		wl_event_source_remove(bt_src);
		bt_src = NULL;
		sd_bus_unref(bt_bus);
		bt_bus = NULL;
		memset(&bt_adapter, 0, sizeof(bt_adapter));
		bt_ndevs = 0;
		btsys_changed();
		return 0;
	}
	while (sd_bus_process(bt_bus, NULL) > 0)
		;
	if (bt_bus && bt_src)
		bus_update_mask(bt_bus, bt_src);
	bus_arm_timer(bt_bus, bt_timer);
	return 0;
}

static int
obex_bus_event(int fd, uint32_t mask, void *data)
{
	if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
		wl_event_source_remove(obex_src);
		obex_src = NULL;
		sd_bus_unref(obex_bus);
		obex_bus = NULL;
		return 0;
	}
	while (sd_bus_process(obex_bus, NULL) > 0)
		;
	if (obex_bus && obex_src)
		bus_update_mask(obex_bus, obex_src);
	bus_arm_timer(obex_bus, obex_timer);
	return 0;
}

/* ── init ────────────────────────────────────────────────────────── */

static int
ignore_reply_cb(sd_bus_message *m, void *userdata, sd_bus_error *err)
{
	return 0;
}

void
btmon_init(void)
{
	int r;

	r = sd_bus_open_system(&bt_bus);
	if (r < 0)
		return;
	/* All calls are async, so a long timeout can't stall the
	 * compositor — and Pair/Connect legitimately take up to ~30s. */
	sd_bus_set_method_call_timeout(bt_bus, 90ULL * 1000000);

	sd_bus_match_signal(bt_bus, NULL, "org.bluez", "/",
			"org.freedesktop.DBus.ObjectManager", "InterfacesAdded",
			interfaces_added_cb, NULL);
	sd_bus_match_signal(bt_bus, NULL, "org.bluez", "/",
			"org.freedesktop.DBus.ObjectManager", "InterfacesRemoved",
			interfaces_removed_cb, NULL);
	sd_bus_add_match(bt_bus, NULL,
			"type='signal',sender='org.bluez',"
			"interface='org.freedesktop.DBus.Properties',"
			"member='PropertiesChanged',path_namespace='/org/bluez'",
			props_changed_cb, NULL);

	sd_bus_add_object_vtable(bt_bus, NULL, BT_AGENT_PATH,
			"org.bluez.Agent1", agent_vtable, NULL);
	sd_bus_call_method_async(bt_bus, NULL, "org.bluez", "/org/bluez",
			"org.bluez.AgentManager1", "RegisterAgent",
			ignore_reply_cb, NULL, "os", BT_AGENT_PATH,
			"KeyboardDisplay");
	sd_bus_call_method_async(bt_bus, NULL, "org.bluez", "/org/bluez",
			"org.bluez.AgentManager1", "RequestDefaultAgent",
			ignore_reply_cb, NULL, "o", BT_AGENT_PATH);

	sd_bus_call_method_async(bt_bus, NULL, "org.bluez", "/",
			"org.freedesktop.DBus.ObjectManager", "GetManagedObjects",
			managed_objects_cb, NULL, "");

	bt_src = wl_event_loop_add_fd(event_loop, sd_bus_get_fd(bt_bus),
			WL_EVENT_READABLE, bt_bus_event, NULL);
	if (bt_src)
		bus_update_mask(bt_bus, bt_src);
	bt_timer = wl_event_loop_add_timer(event_loop, bt_timer_cb, NULL);
	bus_arm_timer(bt_bus, bt_timer);

	/* OBEX: session bus; obexd is D-Bus activated on first use */
	if (sd_bus_open_user(&obex_bus) >= 0) {
		sd_bus_set_method_call_timeout(obex_bus, 90ULL * 1000000);
		sd_bus_add_object_vtable(obex_bus, NULL, OBEX_AGENT_PATH,
				"org.bluez.obex.Agent1", obex_agent_vtable, NULL);
		sd_bus_call_method_async(obex_bus, NULL, "org.bluez.obex",
				"/org/bluez/obex", "org.bluez.obex.AgentManager1",
				"RegisterAgent", ignore_reply_cb, NULL, "o",
				OBEX_AGENT_PATH);
		obex_src = wl_event_loop_add_fd(event_loop,
				sd_bus_get_fd(obex_bus), WL_EVENT_READABLE,
				obex_bus_event, NULL);
		if (obex_src)
			bus_update_mask(obex_bus, obex_src);
		obex_timer = wl_event_loop_add_timer(event_loop,
				obex_timer_cb, NULL);
		bus_arm_timer(obex_bus, obex_timer);
	}
}

/* ── public API ──────────────────────────────────────────────────── */

int
btmon_adapter(BtAdapter *out)
{
	*out = bt_adapter;
	return bt_adapter.present;
}

int
btmon_devices(BtDev *out, int max)
{
	int n = bt_ndevs < max ? bt_ndevs : max;
	int i, j;

	memcpy(out, bt_devs, n * sizeof(BtDev));
	/* connected first, then paired, then by RSSI */
	for (i = 0; i < n; i++)
		for (j = i + 1; j < n; j++) {
			int si = out[i].connected * 4 + out[i].paired * 2;
			int sj = out[j].connected * 4 + out[j].paired * 2;

			if (sj > si || (sj == si &&
						out[j].rssi > out[i].rssi)) {
				BtDev t = out[i];
				out[i] = out[j];
				out[j] = t;
			}
		}
	return n;
}

void
btmon_set_powered(int on)
{
	if (!bt_bus || !bt_adapter_path[0])
		return;
	sd_bus_call_method_async(bt_bus, NULL, "org.bluez", bt_adapter_path,
			"org.freedesktop.DBus.Properties", "Set",
			ignore_reply_cb, NULL, "ssv",
			"org.bluez.Adapter1", "Powered", "b", on);
}

void
btmon_set_discovering(int on)
{
	if (!bt_bus || !bt_adapter_path[0])
		return;
	sd_bus_call_method_async(bt_bus, NULL, "org.bluez", bt_adapter_path,
			"org.bluez.Adapter1",
			on ? "StartDiscovery" : "StopDiscovery",
			ignore_reply_cb, NULL, "");
}

static int
pair_done_cb(sd_bus_message *m, void *userdata, sd_bus_error *err)
{
	char *path = userdata;

	if (!sd_bus_message_is_method_error(m, NULL)) {
		/* trust + connect once paired */
		sd_bus_call_method_async(bt_bus, NULL, "org.bluez", path,
				"org.freedesktop.DBus.Properties", "Set",
				ignore_reply_cb, NULL, "ssv",
				"org.bluez.Device1", "Trusted", "b", 1);
		sd_bus_call_method_async(bt_bus, NULL, "org.bluez", path,
				"org.bluez.Device1", "Connect",
				ignore_reply_cb, NULL, "");
	} else {
		bt_osd("Bluetooth pairing failed");
	}
	free(path);
	btsys_changed();
	return 0;
}

void
btmon_pair(const char *path)
{
	char *copy;

	if (!bt_bus)
		return;
	copy = strdup(path);
	if (!copy)
		return;
	if (sd_bus_call_method_async(bt_bus, NULL, "org.bluez", path,
			"org.bluez.Device1", "Pair",
			pair_done_cb, copy, "") < 0)
		free(copy);
}

void
btmon_connect(const char *path)
{
	if (!bt_bus)
		return;
	sd_bus_call_method_async(bt_bus, NULL, "org.bluez", path,
			"org.bluez.Device1", "Connect",
			ignore_reply_cb, NULL, "");
}

void
btmon_disconnect(const char *path)
{
	if (!bt_bus)
		return;
	sd_bus_call_method_async(bt_bus, NULL, "org.bluez", path,
			"org.bluez.Device1", "Disconnect",
			ignore_reply_cb, NULL, "");
}

void
btmon_remove(const char *path)
{
	if (!bt_bus || !bt_adapter_path[0])
		return;
	sd_bus_call_method_async(bt_bus, NULL, "org.bluez", bt_adapter_path,
			"org.bluez.Adapter1", "RemoveDevice",
			ignore_reply_cb, NULL, "o", path);
}

/* ── OBEX send ───────────────────────────────────────────────────── */

static char obex_pending_file[512];

static int
obex_session_cb(sd_bus_message *m, void *userdata, sd_bus_error *err)
{
	const char *session;

	if (sd_bus_message_is_method_error(m, NULL)) {
		bt_osd("Bluetooth: file transfer failed");
		return 0;
	}
	if (sd_bus_message_read(m, "o", &session) < 0)
		return 0;
	sd_bus_call_method_async(obex_bus, NULL, "org.bluez.obex", session,
			"org.bluez.obex.ObjectPush1", "SendFile",
			ignore_reply_cb, NULL, "s", obex_pending_file);
	bt_osd("Bluetooth: sending file");
	return 0;
}

int
btmon_send_file(const char *addr, const char *filepath)
{
	sd_bus_message *req = NULL;
	int r;

	if (!obex_bus)
		return -1;
	snprintf(obex_pending_file, sizeof(obex_pending_file), "%s", filepath);
	r = sd_bus_message_new_method_call(obex_bus, &req, "org.bluez.obex",
			"/org/bluez/obex", "org.bluez.obex.Client1",
			"CreateSession");
	if (r < 0)
		return -1;
	sd_bus_message_append(req, "s", addr);
	sd_bus_message_open_container(req, 'a', "{sv}");
	sd_bus_message_open_container(req, 'e', "sv");
	sd_bus_message_append(req, "s", "Target");
	sd_bus_message_append(req, "v", "s", "opp");
	sd_bus_message_close_container(req);
	sd_bus_message_close_container(req);
	r = sd_bus_call_async(obex_bus, NULL, req, obex_session_cb, NULL, 0);
	sd_bus_message_unref(req);
	return r < 0 ? -1 : 0;
}
