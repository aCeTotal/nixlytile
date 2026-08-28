/* VPN profiles = systemd units.  Any wg-quick / openvpn / openconnect /
 * strongswan / tailscale unit on the system shows up as a profile; the
 * popup starts/stops them via systemctl (authorized by a polkit rule
 * for the wheel group) and "autoconnect" is simply the unit's enabled
 * state.  No daemon of our own, nothing resident: two async systemctl
 * reads when the popup asks for a refresh.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fetch_async.h"
#include "netsys.h"

static VpnProfile vpn[VPN_MAX];
static int vpn_count;
static int vpn_listing;

static const char vpn_list_cmd[] =
	"systemctl list-unit-files --no-legend --plain "
	"'wg-quick@*.service' 'wg-quick-*.service' 'openvpn-*.service' "
	"'openconnect-*.service' 'openfortivpn-*.service' "
	"'strongswan.service' 'strongswan-swanctl.service' "
	"'tailscaled.service' 2>/dev/null";

static void
vpn_make_label(VpnProfile *p)
{
	const char *u = p->unit;
	char name[64];

	if (strncmp(u, "wg-quick@", 9) == 0 || strncmp(u, "wg-quick-", 9) == 0) {
		snprintf(name, sizeof(name), "%s", u + 9);
		name[strcspn(name, ".")] = '\0';
		snprintf(p->label, sizeof(p->label), "%s · WireGuard", name);
	} else if (strncmp(u, "openvpn-", 8) == 0) {
		snprintf(name, sizeof(name), "%s", u + 8);
		name[strcspn(name, ".")] = '\0';
		snprintf(p->label, sizeof(p->label), "%s · OpenVPN", name);
	} else if (strncmp(u, "openconnect-", 12) == 0) {
		snprintf(name, sizeof(name), "%s", u + 12);
		name[strcspn(name, ".")] = '\0';
		snprintf(p->label, sizeof(p->label), "%s · OpenConnect", name);
	} else if (strncmp(u, "openfortivpn-", 13) == 0) {
		snprintf(name, sizeof(name), "%s", u + 13);
		name[strcspn(name, ".")] = '\0';
		snprintf(p->label, sizeof(p->label), "%s · Fortinet", name);
	} else if (strncmp(u, "strongswan", 10) == 0) {
		snprintf(p->label, sizeof(p->label), "IPsec · strongSwan");
	} else if (strncmp(u, "tailscaled", 10) == 0) {
		snprintf(p->label, sizeof(p->label), "Tailscale");
	} else {
		snprintf(p->label, sizeof(p->label), "%s", u);
	}
}

static void
vpn_active_done(const char *out, size_t len, void *data)
{
	char buf[2048];
	char *line, *save;
	int i = 0;

	vpn_listing = 0;
	if (!out)
		return;
	snprintf(buf, sizeof(buf), "%.*s", (int)(len < sizeof(buf) - 1 ?
				len : sizeof(buf) - 1), out);
	line = strtok_r(buf, "\n", &save);
	while (line && i < vpn_count) {
		vpn[i].active = strcmp(line, "active") == 0;
		vpn[i].busy = 0;
		i++;
		line = strtok_r(NULL, "\n", &save);
	}
	netsys_changed();
}

static void
vpn_list_done(const char *out, size_t len, void *data)
{
	char buf[4096];
	char cmd[1536];
	char *line, *save;
	size_t pos;
	int i;

	vpn_count = 0;
	if (!out || len == 0) {
		vpn_listing = 0;
		netsys_changed();
		return;
	}
	snprintf(buf, sizeof(buf), "%.*s", (int)(len < sizeof(buf) - 1 ?
				len : sizeof(buf) - 1), out);
	line = strtok_r(buf, "\n", &save);
	while (line && vpn_count < VPN_MAX) {
		char unit[96], state[32];

		if (sscanf(line, "%95s %31s", unit, state) == 2 &&
				strstr(unit, ".service") &&
				!strstr(unit, "@.service")) { /* skip bare templates */
			VpnProfile *p = &vpn[vpn_count++];

			memset(p, 0, sizeof(*p));
			snprintf(p->unit, sizeof(p->unit), "%s", unit);
			p->autoconnect = strcmp(state, "enabled") == 0;
			vpn_make_label(p);
		}
		line = strtok_r(NULL, "\n", &save);
	}
	if (!vpn_count) {
		vpn_listing = 0;
		netsys_changed();
		return;
	}
	pos = snprintf(cmd, sizeof(cmd), "systemctl is-active");
	for (i = 0; i < vpn_count && pos < sizeof(cmd) - 100; i++)
		pos += snprintf(cmd + pos, sizeof(cmd) - pos, " %s",
				vpn[i].unit);
	if (fetch_async(cmd, vpn_active_done, NULL) != 0)
		vpn_listing = 0;
}

void
vpnctl_refresh(void)
{
	if (vpn_listing)
		return;
	vpn_listing = 1;
	if (fetch_async(vpn_list_cmd, vpn_list_done, NULL) != 0)
		vpn_listing = 0;
}

void
vpnctl_init(void)
{
	vpnctl_refresh();
}

int
vpnctl_profiles(VpnProfile *out, int max)
{
	int n = vpn_count < max ? vpn_count : max;

	memcpy(out, vpn, n * sizeof(VpnProfile));
	return n;
}

static void
vpn_op_done(const char *out, size_t len, void *data)
{
	int i;

	/* vpnctl_refresh may be a no-op if a listing is already in flight,
	 * and busy is otherwise only cleared by vpn_active_done — don't
	 * leave a row stuck on "…" */
	for (i = 0; i < vpn_count; i++)
		vpn[i].busy = 0;
	vpnctl_refresh();
}

void
vpnctl_toggle(int idx)
{
	char cmd[160];

	if (idx < 0 || idx >= vpn_count || vpn[idx].busy)
		return;
	vpn[idx].busy = 1;
	snprintf(cmd, sizeof(cmd), "systemctl %s %s 2>/dev/null",
			vpn[idx].active ? "stop" : "start", vpn[idx].unit);
	if (fetch_async(cmd, vpn_op_done, NULL) != 0)
		vpn[idx].busy = 0;
}

void
vpnctl_set_autoconnect(int idx, int on)
{
	char cmd[160];

	if (idx < 0 || idx >= vpn_count)
		return;
	vpn[idx].autoconnect = on;   /* optimistic; refresh confirms */
	snprintf(cmd, sizeof(cmd), "systemctl %s %s 2>/dev/null",
			on ? "enable" : "disable", vpn[idx].unit);
	fetch_async(cmd, vpn_op_done, NULL);
}
