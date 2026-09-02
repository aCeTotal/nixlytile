/* Hover QR detection: when the cursor rests over a client surface, a
 * small region around it is read back from the surface texture and
 * scanned for a Wi-Fi QR code (the kind Share Wi-Fi generates).  On a
 * hit a one-row overlay popup appears by the cursor — "Connect to
 * SSID: xxx" — and clicking it joins the network with the embedded
 * credentials.
 *
 * Cost control: the readback (≈1 MB GPU→CPU) plus quirc decode runs
 * only after the cursor has been stationary ~450 ms, and never twice
 * for the same resting spot.
 */
#include <drm_fourcc.h>
#include <math.h>
#include <quirc.h>
#include <stdlib.h>
#include <string.h>

#include "nixlytile.h"
#include "client.h"
#include "netsys.h"
#include "popup_card.h"

#define QS_IDLE_MS   450
#define QS_JITTER_PX 24    /* movement below this keeps the same scan spot */
#define QS_MARGIN    32    /* leave-popup dismiss margin */

static struct wl_event_source *qs_timer;
static double qs_scan_x = -1e9, qs_scan_y = -1e9;

/* decoded network */
static char qs_ssid[33];
static char qs_psk[80];
static int qs_hidden;

/* popup */
static struct wlr_scene_tree *qs_tree;
static struct wlr_scene_buffer *qs_buf;
static int qs_x, qs_y, qs_w, qs_h;
static int qs_hot;

static void
qs_popup_hide(void)
{
	if (!qs_tree)
		return;
	wlr_scene_node_destroy(&qs_tree->node);
	qs_tree = NULL;
	qs_buf = NULL;
	qs_w = qs_h = 0;
	qs_hot = 0;
}

static void
qs_popup_render(void)
{
	Card *card;
	CardResult res;
	char label[64];

	card = card_begin();
	if (!card)
		return;
	card_min_w(card, 60);
	snprintf(label, sizeof(label), "Connect to SSID: %s", qs_ssid);
	card_icon_text(card, wifi_icon_for_quality(100.0), label, NULL,
			1, qs_hot);
	if (card_finish(card, &res) != 0)
		return;
	if (!qs_tree) {
		qs_tree = wlr_scene_tree_create(layers[LyrOverlay]);
		if (!qs_tree) {
			wlr_buffer_drop(res.buf);
			return;
		}
		qs_buf = wlr_scene_buffer_create(qs_tree, NULL);
	}
	if (qs_buf)
		wlr_scene_buffer_set_buffer(qs_buf, res.buf);
	wlr_buffer_drop(res.buf);
	qs_w = res.w;
	qs_h = res.h;
	wlr_scene_node_set_position(&qs_tree->node, qs_x, qs_y);
}

static void
qs_popup_show(void)
{
	Monitor *m = selmon;

	qs_x = (int)cursor->x + 16;
	qs_y = (int)cursor->y + 16;
	qs_popup_render();
	if (!qs_tree)
		return;
	/* keep the card on the monitor */
	if (m) {
		if (qs_x + qs_w > m->m.x + m->m.width)
			qs_x = m->m.x + m->m.width - qs_w;
		if (qs_y + qs_h > m->m.y + m->m.height)
			qs_y = m->m.y + m->m.height - qs_h;
		wlr_scene_node_set_position(&qs_tree->node, qs_x, qs_y);
	}
	if (m && m->wlr_output)
		wlr_output_schedule_frame(m->wlr_output);
}

/* WIFI:T:WPA;S:<ssid>;P:<psk>;H:true;; with \-escaped \ ; , : " */
static int
wifi_qr_parse(const char *s)
{
	char val[128];
	int open_net = 0;

	if (strncmp(s, "WIFI:", 5) != 0)
		return -1;
	s += 5;
	qs_ssid[0] = qs_psk[0] = '\0';
	qs_hidden = 0;
	while (*s && *s != ';') {
		char key = *s;
		size_t o = 0;

		if (s[1] != ':')
			return -1;
		s += 2;
		while (*s && *s != ';') {
			char ch = *s++;

			if (ch == '\\' && *s)
				ch = *s++;
			if (o + 1 < sizeof(val))
				val[o++] = ch;
		}
		val[o] = '\0';
		if (*s == ';')
			s++;
		if (key == 'S')
			snprintf(qs_ssid, sizeof(qs_ssid), "%s", val);
		else if (key == 'P')
			snprintf(qs_psk, sizeof(qs_psk), "%s", val);
		else if (key == 'H')
			qs_hidden = strcmp(val, "true") == 0;
		else if (key == 'T')
			open_net = strcmp(val, "nopass") == 0;
	}
	if (!qs_ssid[0])
		return -1;
	if (open_net)
		qs_psk[0] = '\0';
	return 0;
}

static int
qs_decode(const uint8_t *px, int w, int h)
{
	struct quirc *q;
	uint8_t *img;
	int i, n, found = -1;

	q = quirc_new();
	if (!q || quirc_resize(q, w, h) < 0) {
		quirc_destroy(q);
		return -1;
	}
	img = quirc_begin(q, NULL, NULL);
	for (i = 0; i < w * h; i++) {
		uint32_t p;

		memcpy(&p, px + (size_t)i * 4, 4);
		/* luma from any 32-bit RGB layout: R/B swap is sum-neutral */
		img[i] = (uint8_t)(((p & 0xff) + (p >> 8 & 0xff) +
				(p >> 16 & 0xff)) / 3);
	}
	quirc_end(q);
	n = quirc_count(q);
	for (i = 0; i < n && found < 0; i++) {
		struct quirc_code code;
		struct quirc_data data;

		quirc_extract(q, i, &code);
		if (quirc_decode(&code, &data) != 0) {
			quirc_flip(&code);
			if (quirc_decode(&code, &data) != 0)
				continue;
		}
		if (wifi_qr_parse((const char *)data.payload) == 0)
			found = 0;
	}
	quirc_destroy(q);
	return found;
}

static int
qs_idle(void *data)
{
	struct wlr_surface *surface = NULL;
	Client *c = NULL;
	struct wlr_client_buffer *cb;
	struct wlr_texture *tex;
	double sx = 0, sy = 0;
	int bx, by, r, x0, y0, w, h;
	uint32_t fmt;
	void *px;

	(void)data;
	qs_scan_x = cursor->x;
	qs_scan_y = cursor->y;
	if (locked || cursor_mode != CurNormal || active_constraint)
		return 0;
	if (selmon && statusbar_popup_at(selmon, cursor->x, cursor->y))
		return 0;
	xytonode(cursor->x, cursor->y, &surface, &c, NULL, &sx, &sy);
	if (!surface || !c)
		return 0;
	cb = surface->buffer;
	if (!cb || !cb->texture)
		return 0;
	tex = cb->texture;

	bx = (int)(sx * surface->current.scale);
	by = (int)(sy * surface->current.scale);
	r = (int)(240 * surface->current.scale);
	x0 = bx - r;
	y0 = by - r;
	if (x0 < 0)
		x0 = 0;
	if (y0 < 0)
		y0 = 0;
	w = bx + r > (int)tex->width ? (int)tex->width - x0 : bx + r - x0;
	h = by + r > (int)tex->height ? (int)tex->height - y0 : by + r - y0;
	if (w < 64 || h < 64)
		return 0;

	fmt = wlr_texture_preferred_read_format(tex);
	if (fmt != DRM_FORMAT_XRGB8888 && fmt != DRM_FORMAT_ARGB8888 &&
			fmt != DRM_FORMAT_XBGR8888 && fmt != DRM_FORMAT_ABGR8888)
		fmt = DRM_FORMAT_ARGB8888;
	px = malloc((size_t)w * h * 4);
	if (!px)
		return 0;
	if (!wlr_texture_read_pixels(tex,
			&(struct wlr_texture_read_pixels_options){
				.data = px,
				.format = fmt,
				.stride = (uint32_t)w * 4,
				.src_box = { x0, y0, w, h },
			})) {
		free(px);
		return 0;
	}
	if (qs_decode(px, w, h) == 0)
		qs_popup_show();
	free(px);
	return 0;
}

static int
qs_inside(double cx, double cy, int margin)
{
	return cx >= qs_x - margin && cy >= qs_y - margin &&
		cx < qs_x + qs_w + margin && cy < qs_y + qs_h + margin;
}

int
qr_scan_popup_at(double cx, double cy)
{
	return qs_tree && qs_inside(cx, cy, 0);
}

void
qr_scan_motion(uint32_t time)
{
	if (!time)
		return;
	if (qs_tree) {
		if (qs_inside(cursor->x, cursor->y, QS_MARGIN)) {
			int hot = qs_inside(cursor->x, cursor->y, 0);

			if (hot != qs_hot) {
				qs_hot = hot;
				qs_popup_render();
				if (selmon && selmon->wlr_output)
					wlr_output_schedule_frame(
							selmon->wlr_output);
			}
			return;
		}
		qs_popup_hide();
		if (selmon && selmon->wlr_output)
			wlr_output_schedule_frame(selmon->wlr_output);
	}
	if (fabs(cursor->x - qs_scan_x) < QS_JITTER_PX &&
			fabs(cursor->y - qs_scan_y) < QS_JITTER_PX)
		return;
	if (!qs_timer)
		qs_timer = wl_event_loop_add_timer(event_loop, qs_idle, NULL);
	if (qs_timer)
		wl_event_source_timer_update(qs_timer, QS_IDLE_MS);
}

int
qr_scan_handle_click(double cx, double cy, uint32_t button)
{
	if (!qs_tree || !qs_inside(cx, cy, 0))
		return 0;
	if (button == BTN_LEFT && qs_ssid[0])
		wifi_connect(qs_ssid, qs_psk, qs_hidden);
	qs_popup_hide();
	return 1;
}
