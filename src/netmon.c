/* Link monitor: one rtnetlink socket in the event loop replaces all
 * link-state polling.  Interface inventory + carrier come from sysfs
 * (cheap, event-driven re-reads only), link speed/duplex from the
 * ethtool ioctl, radio policy from /dev/rfkill.
 *
 * Policy: when the ethernet link has carrier the wifi radio is
 * soft-blocked, unless the user explicitly turned wifi on from the
 * popup.  Losing ethernet carrier unblocks wifi again.
 */
#define _DEFAULT_SOURCE   /* struct ifreq */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/ethtool.h>
#include <linux/netlink.h>
#include <linux/rfkill.h>
#include <linux/rtnetlink.h>
#include <linux/sockios.h>
#include <net/if.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wayland-server-core.h>

#include "netsys.h"

extern struct wl_event_loop *event_loop;
void wifi_ctrl_sync(void);

static NetLinkSnap nm_snap;
static int nm_policy_blocked;   /* the rfkill block is ours, not the user's */
static int nm_user_off;         /* user explicitly disabled the radio */
static int nm_rtnl_fd = -1;
static struct wl_event_source *nm_rtnl_src;
static struct wl_event_source *nm_settle_timer;

static int
read_sysfs_str(const char *iface, const char *file, char *out, size_t len)
{
	char path[256];
	FILE *f;

	snprintf(path, sizeof(path), "/sys/class/net/%s/%s", iface, file);
	f = fopen(path, "r");
	if (!f)
		return -1;
	if (!fgets(out, len, f)) {
		fclose(f);
		return -1;
	}
	fclose(f);
	out[strcspn(out, "\n")] = '\0';
	return 0;
}

static int
iface_wireless(const char *iface)
{
	char path[256];
	struct stat st;

	snprintf(path, sizeof(path), "/sys/class/net/%s/wireless", iface);
	return stat(path, &st) == 0;
}

static void
ethtool_speed(NetLink *l)
{
	struct ifreq ifr;
	struct ethtool_cmd cmd;
	int fd;

	l->speed_mbps = -1;
	l->duplex_full = -1;
	fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return;
	memset(&ifr, 0, sizeof(ifr));
	snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", l->iface);
	memset(&cmd, 0, sizeof(cmd));
	cmd.cmd = ETHTOOL_GSET;
	ifr.ifr_data = (void *)&cmd;
	if (ioctl(fd, SIOCETHTOOL, &ifr) == 0) {
		unsigned speed = ethtool_cmd_speed(&cmd);
		if (speed != (unsigned)SPEED_UNKNOWN && speed > 0 && speed < 400000)
			l->speed_mbps = (int)speed;
		if (cmd.duplex != DUPLEX_UNKNOWN)
			l->duplex_full = cmd.duplex == DUPLEX_FULL;
	}
	close(fd);
	if (l->speed_mbps < 0) {
		char buf[32];
		int sp;
		if (read_sysfs_str(l->iface, "speed", buf, sizeof(buf)) == 0 &&
				sscanf(buf, "%d", &sp) == 1 && sp > 0 && sp < 400000)
			l->speed_mbps = sp;
	}
}

static void
fill_link(NetLink *l)
{
	char buf[64];

	l->carrier = 0;
	l->mac[0] = '\0';
	if (read_sysfs_str(l->iface, "carrier", buf, sizeof(buf)) == 0)
		l->carrier = buf[0] == '1';
	if (read_sysfs_str(l->iface, "address", buf, sizeof(buf)) == 0)
		snprintf(l->mac, sizeof(l->mac), "%s", buf);
	if (l->carrier)
		ethtool_speed(l);
	else {
		l->speed_mbps = -1;
		l->duplex_full = -1;
	}
}

static int
wifi_soft_blocked(void)
{
	struct rfkill_event ev;
	int fd, blocked = 0;
	ssize_t n;

	fd = open("/dev/rfkill", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		return 0;
	while ((n = read(fd, &ev, sizeof(ev))) > 0) {
		/* hard block (airplane-mode switch) counts too: the radio
		 * is just as dead, only not toggleable from software */
		if (n >= (ssize_t)RFKILL_EVENT_SIZE_V1 &&
				ev.type == RFKILL_TYPE_WLAN &&
				(ev.soft || ev.hard))
			blocked = 1;
	}
	close(fd);
	return blocked;
}

static void
rfkill_wifi(int block)
{
	struct rfkill_event ev;
	int fd;

	fd = open("/dev/rfkill", O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		return;
	memset(&ev, 0, sizeof(ev));
	ev.op = RFKILL_OP_CHANGE_ALL;
	ev.type = RFKILL_TYPE_WLAN;
	ev.soft = block ? 1 : 0;
	(void)!write(fd, &ev, RFKILL_EVENT_SIZE_V1);
	close(fd);
}

/* Re-inventory interfaces and apply the radio policy. */
static void
netmon_rescan(void)
{
	DIR *d;
	struct dirent *de;
	int had_eth_carrier = nm_snap.eth.carrier;

	nm_snap.eth.present = nm_snap.wifi.present = 0;
	d = opendir("/sys/class/net");
	if (!d)
		return;
	while ((de = readdir(d))) {
		char buf[32];

		if (de->d_name[0] == '.' || strcmp(de->d_name, "lo") == 0)
			continue;
		/* skip virtual devices (tailscale0, wg*, tun*): no /device dir */
		{
			char path[256];
			struct stat st;
			snprintf(path, sizeof(path),
					"/sys/class/net/%s/device", de->d_name);
			if (stat(path, &st) != 0)
				continue;
		}
		if (read_sysfs_str(de->d_name, "type", buf, sizeof(buf)) != 0 ||
				strcmp(buf, "1") != 0)
			continue;   /* not ARPHRD_ETHER */
		if (iface_wireless(de->d_name)) {
			if (!nm_snap.wifi.present) {
				nm_snap.wifi.present = 1;
				snprintf(nm_snap.wifi.iface,
						sizeof(nm_snap.wifi.iface),
						"%s", de->d_name);
			}
		} else if (!nm_snap.eth.present) {
			nm_snap.eth.present = 1;
			snprintf(nm_snap.eth.iface, sizeof(nm_snap.eth.iface),
					"%s", de->d_name);
		}
	}
	closedir(d);

	if (nm_snap.eth.present) {
		fill_link(&nm_snap.eth);
	} else {
		/* NIC unplugged: don't let a phantom carrier linger */
		nm_snap.eth.carrier = 0;
		nm_snap.eth.speed_mbps = -1;
		nm_snap.eth.mac[0] = '\0';
	}
	if (nm_snap.wifi.present) {
		fill_link(&nm_snap.wifi);
	} else {
		nm_snap.wifi.carrier = 0;
		nm_snap.wifi.speed_mbps = -1;
		nm_snap.wifi.mac[0] = '\0';
	}
	nm_snap.wifi_blocked = wifi_soft_blocked();

	/* wifi-off-on-ethernet policy; an explicit user choice (either
	 * direction) always wins over the automatic rule.  Stands down
	 * entirely while NetworkManager runs — NM owns radio policy and
	 * a surprise rfkill block would cut its wifi connection. */
	if (!nm_backend_active() &&
			nm_snap.eth.present && nm_snap.wifi.present) {
		if (nm_snap.eth.carrier && !nm_snap.wifi_user_on &&
				!nm_snap.wifi_blocked) {
			rfkill_wifi(1);
			nm_snap.wifi_blocked = 1;
			nm_policy_blocked = 1;
		} else if (!nm_snap.eth.carrier && nm_snap.wifi_blocked &&
				!nm_user_off &&
				(had_eth_carrier || nm_policy_blocked)) {
			/* cable pulled (or our block went stale): bring wifi
			 * back so the machine is never left offline */
			rfkill_wifi(0);
			nm_snap.wifi_blocked = 0;
			nm_policy_blocked = 0;
		}
	}

	wifi_ctrl_sync();
}

/* Carrier flaps arrive in bursts (auto-negotiation); settle 300ms then
 * rescan once. */
static int
nm_settle(void *data)
{
	netmon_rescan();
	netsys_changed();
	return 0;
}

static int
nm_rtnl_event(int fd, uint32_t mask, void *data)
{
	char buf[4096];

	if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
		wl_event_source_remove(nm_rtnl_src);
		nm_rtnl_src = NULL;
		close(nm_rtnl_fd);
		nm_rtnl_fd = -1;
		return 0;
	}
	while (recv(fd, buf, sizeof(buf), MSG_DONTWAIT) > 0)
		;
	if (nm_settle_timer)
		wl_event_source_timer_update(nm_settle_timer, 300);
	return 0;
}

void
netmon_init(void)
{
	struct sockaddr_nl addr;

	nm_rtnl_fd = socket(AF_NETLINK,
			SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, NETLINK_ROUTE);
	if (nm_rtnl_fd >= 0) {
		memset(&addr, 0, sizeof(addr));
		addr.nl_family = AF_NETLINK;
		addr.nl_groups = RTMGRP_LINK | RTMGRP_IPV4_IFADDR;
		if (bind(nm_rtnl_fd, (struct sockaddr *)&addr,
					sizeof(addr)) == 0) {
			nm_rtnl_src = wl_event_loop_add_fd(event_loop,
					nm_rtnl_fd, WL_EVENT_READABLE,
					nm_rtnl_event, NULL);
		} else {
			close(nm_rtnl_fd);
			nm_rtnl_fd = -1;
		}
	}
	nm_settle_timer = wl_event_loop_add_timer(event_loop, nm_settle, NULL);
	netmon_rescan();
	/* A soft-block left over from a previous session's policy would
	 * strand the machine offline when the cable is gone at boot. */
	if (!nm_backend_active() &&
			nm_snap.wifi.present && nm_snap.wifi_blocked &&
			!(nm_snap.eth.present && nm_snap.eth.carrier)) {
		rfkill_wifi(0);
		nm_snap.wifi_blocked = 0;
		wifi_ctrl_sync();
	}
}

void
netmon_get(NetLinkSnap *out)
{
	*out = nm_snap;
}

int
netmon_stats(const char *iface, NetIfStats *out)
{
	/* rx_dropped is deliberately not read: on wifi it also counts
	 * mac80211 protocol discards (foreign broadcasts, group mgmt
	 * frames) that never affect traffic.  Only ring/buffer overruns
	 * are real rx loss. */
	static const char *files[] = {
		"rx_bytes", "tx_bytes", "rx_packets", "tx_packets",
		"rx_errors", "tx_errors", "rx_missed_errors", "tx_dropped",
	};
	static const char *extra[] = { "rx_over_errors", "rx_fifo_errors" };
	unsigned long long *vals = &out->rx_bytes;
	size_t i;

	memset(out, 0, sizeof(*out));
	for (i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
		char path[256], buf[32];
		FILE *f;

		snprintf(path, sizeof(path),
				"/sys/class/net/%s/statistics/%s", iface, files[i]);
		f = fopen(path, "r");
		if (!f)
			return -1;
		if (fgets(buf, sizeof(buf), f))
			sscanf(buf, "%llu", &vals[i]);
		fclose(f);
	}
	for (i = 0; i < sizeof(extra) / sizeof(extra[0]); i++) {
		char path[256], buf[32];
		unsigned long long v = 0;
		FILE *f;

		snprintf(path, sizeof(path),
				"/sys/class/net/%s/statistics/%s", iface, extra[i]);
		f = fopen(path, "r");
		if (!f)
			continue;
		if (fgets(buf, sizeof(buf), f) && sscanf(buf, "%llu", &v) == 1)
			out->rx_dropped += v;
		fclose(f);
	}
	return 0;
}

int
netmon_gateway(char *out, size_t len)
{
	FILE *f;
	char line[256];

	out[0] = '\0';
	f = fopen("/proc/net/route", "r");
	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f)) {
		char ifn[IF_NAMESIZE];
		unsigned long dest, gw;

		if (sscanf(line, "%15s %lx %lx", ifn, &dest, &gw) == 3 &&
				dest == 0 && gw != 0) {
			snprintf(out, len, "%lu.%lu.%lu.%lu",
					gw & 0xff, (gw >> 8) & 0xff,
					(gw >> 16) & 0xff, (gw >> 24) & 0xff);
			break;
		}
	}
	fclose(f);
	return out[0] ? 0 : -1;
}

int
netmon_dns(char *out, size_t len)
{
	FILE *f;
	char line[256];

	out[0] = '\0';
	/* systemd-resolved: the real upstream is in its own resolv.conf */
	f = fopen("/run/systemd/resolve/resolv.conf", "r");
	if (!f)
		f = fopen("/etc/resolv.conf", "r");
	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f)) {
		char ns[128];

		if (sscanf(line, "nameserver %127s", ns) == 1) {
			if (out[0])
				snprintf(out + strlen(out), len - strlen(out),
						", %s", ns);
			else
				snprintf(out, len, "%s", ns);
			if (strlen(out) > len - 20)
				break;
		}
	}
	fclose(f);
	return out[0] ? 0 : -1;
}

void
netmon_wifi_set_enabled(int on, int user)
{
	rfkill_wifi(!on);
	nm_snap.wifi_blocked = !on;
	if (user) {
		nm_snap.wifi_user_on = on;
		nm_user_off = !on;
		nm_policy_blocked = 0;
	}
	if (nm_settle_timer)
		wl_event_source_timer_update(nm_settle_timer, 400);
}
