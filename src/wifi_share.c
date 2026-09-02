/* Share Wi-Fi: QR code for the currently connected network.
 *
 * The passphrase is read from wpa_supplicant's saved config (written by
 * SAVE_CONFIG on successful connects) and embedded in a standard
 * "WIFI:T:WPA;S:<ssid>;P:<psk>;;" payload, so another device can join
 * by scanning without the password ever being shown.  "Copy image"
 * renders the code to a PNG under XDG_RUNTIME_DIR and hands it to
 * wl-copy as image/png.
 */
#include <cairo.h>
#include <qrencode.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "netsys.h"
#include "run_cmd.h"
#include "util.h"

#define WPA_CONF "/etc/wpa_supplicant/imperative.conf"

static char sh_ssid[33];
static uint8_t *sh_modules;
static int sh_size;
static char sh_err[64];

const char *
wifi_share_ssid(void)
{
	return sh_ssid;
}

int
wifi_share_active(void)
{
	return sh_ssid[0] != '\0';
}

const uint8_t *
wifi_share_qr(int *size)
{
	*size = sh_size;
	return sh_modules;
}

const char *
wifi_share_status(void)
{
	return sh_err;
}

void
wifi_share_reset(void)
{
	free(sh_modules);
	sh_modules = NULL;
	sh_size = 0;
	sh_ssid[0] = '\0';
	sh_err[0] = '\0';
}

/* Passphrase for `ssid` from the supplicant's saved config; only quoted
 * (plaintext) psk entries are usable in a QR payload. */
static int
psk_lookup(const char *ssid, char *out, size_t len)
{
	FILE *f = fopen(WPA_CONF, "r");
	char line[256], cur[33] = "", psk[80] = "";
	int found = 0;

	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f)) {
		char *p = line;

		while (*p == ' ' || *p == '\t')
			p++;
		if (strncmp(p, "network={", 9) == 0) {
			cur[0] = psk[0] = '\0';
		} else if (sscanf(p, "ssid=\"%32[^\"]\"", cur) == 1) {
			/* captured */
		} else if (sscanf(p, "psk=\"%79[^\"]\"", psk) == 1) {
			/* captured */
		} else if (*p == '}') {
			if (cur[0] && strcmp(cur, ssid) == 0 && psk[0]) {
				snprintf(out, len, "%s", psk);
				found = 1;
				break;
			}
		}
	}
	fclose(f);
	return found ? 0 : -1;
}

/* WIFI: payload escaping: \ ; , : " get a backslash */
static void
qr_escape(const char *s, char *out, size_t len)
{
	size_t o = 0;

	for (; *s && o + 3 < len; s++) {
		if (strchr("\\;,:\"", *s))
			out[o++] = '\\';
		out[o++] = *s;
	}
	out[o] = '\0';
}

void
wifi_share_toggle(const WifiStatus *ws)
{
	char psk[80], essid[80], epsk[176], payload[300];
	QRcode *qr;
	int open_net;

	if (wifi_share_active()) {
		wifi_share_reset();
		return;
	}
	if (!ws || !ws->ssid[0])
		return;
	snprintf(sh_ssid, sizeof(sh_ssid), "%s", ws->ssid);
	sh_err[0] = '\0';

	open_net = !ws->key_mgmt[0] || strstr(ws->key_mgmt, "NONE") != NULL;
	if (!open_net && psk_lookup(ws->ssid, psk, sizeof(psk)) != 0) {
		snprintf(sh_err, sizeof(sh_err),
				"Password not available for sharing");
		return;
	}
	qr_escape(ws->ssid, essid, sizeof(essid));
	if (open_net)
		snprintf(payload, sizeof(payload),
				"WIFI:T:nopass;S:%s;;", essid);
	else {
		qr_escape(psk, epsk, sizeof(epsk));
		snprintf(payload, sizeof(payload),
				"WIFI:T:WPA;S:%s;P:%s;;", essid, epsk);
	}

	qr = QRcode_encodeString8bit(payload, 0, QR_ECLEVEL_M);
	if (!qr) {
		snprintf(sh_err, sizeof(sh_err), "QR encoding failed");
		return;
	}
	sh_size = qr->width;
	sh_modules = ecalloc(1, (size_t)sh_size * sh_size);
	memcpy(sh_modules, qr->data, (size_t)sh_size * sh_size);
	QRcode_free(qr);
}

void
wifi_share_copy(void)
{
	const int scale = 8, margin = 32;
	int px, x, y;
	cairo_surface_t *cs;
	cairo_t *cr;
	char path[256], cmd[300];
	const char *rt = getenv("XDG_RUNTIME_DIR");

	if (!sh_modules)
		return;
	px = sh_size * scale + 2 * margin;
	cs = cairo_image_surface_create(CAIRO_FORMAT_RGB24, px, px);
	if (cairo_surface_status(cs) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(cs);
		return;
	}
	cr = cairo_create(cs);
	cairo_set_source_rgb(cr, 1, 1, 1);
	cairo_paint(cr);
	cairo_set_source_rgb(cr, 0, 0, 0);
	for (y = 0; y < sh_size; y++)
		for (x = 0; x < sh_size; x++)
			if (sh_modules[y * sh_size + x] & 1)
				cairo_rectangle(cr, margin + x * scale,
						margin + y * scale,
						scale, scale);
	cairo_fill(cr);
	cairo_destroy(cr);

	snprintf(path, sizeof(path), "%s/nixlytile-wifi-qr.png",
			rt && rt[0] ? rt : "/tmp");
	if (cairo_surface_write_to_png(cs, path) == CAIRO_STATUS_SUCCESS) {
		const char *argv[] = { "sh", "-c", cmd, NULL };

		snprintf(cmd, sizeof(cmd),
				"wl-copy -t image/png < '%s'", path);
		run_cmd(argv);
	}
	cairo_surface_destroy(cs);
}
