/* Displays statusbar module + popup: proportional monitor strip with
 * drag-to-reorder, plus scale / refresh-rate / rotation controls for
 * the selected output.  Every change is written to
 * ~/.local/nixlyos/monitors.conf — the same file nixlycc's Monitors
 * page writes — and applied instantly by the existing inotify
 * hot-reload in monitors_conf.c.
 */
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "nixlytile.h"
#include "popup_card.h"

#define DHIT_BOX   200   /* + strip index */
#define DHIT_SCALE 220   /* + preset index */
#define DHIT_RATE  230   /* + rate index */
#define DHIT_ROT   240   /* + transform index (normal/90/180/270) */

#define DISP_RATES_MAX 4

char display_icon_path[PATH_MAX] = "images/svg/monitor.svg";
char display_icon_loaded_path[PATH_MAX];
int display_icon_loaded_h, display_icon_w, display_icon_h;
struct wlr_buffer *display_icon_buf;

static const float scale_presets[] = { 1.00f, 1.25f, 1.50f, 1.75f, 2.00f };
#define NSCALES ((int)(sizeof(scale_presets) / sizeof(scale_presets[0])))

typedef struct {
	Monitor *mon;
	char name[64];
	int w, h;               /* mode pixels */
	float hz;
	float scale;
	int transform;          /* WL_OUTPUT_TRANSFORM_* */
	int grid_row;
	char mirror[64];
} DispInfo;

static DispInfo di[CARD_DISP_MAX];
static int ndi;
static int dsel;
static char dsel_name[64];
static float di_rates[DISP_RATES_MAX];
static int di_nrates;

/* drag-to-reorder state (mirrors the volume slider's sdrag pattern) */
static struct {
	int active;
	Monitor *mon;
	int idx;
	double start_cx;        /* layout-x where the press landed */
	int dx;
	int moved;
} ddrag;

/* ── model ───────────────────────────────────────────────────────── */

static int
transform_index(int tr)
{
	switch (tr) {
	case WL_OUTPUT_TRANSFORM_90: return 1;
	case WL_OUTPUT_TRANSFORM_180: return 2;
	case WL_OUTPUT_TRANSFORM_270: return 3;
	default: return 0;
	}
}

static void
disp_logical(const DispInfo *d, float *w, float *h)
{
	int rot = d->transform == WL_OUTPUT_TRANSFORM_90 ||
		d->transform == WL_OUTPUT_TRANSFORM_270;
	float sc = d->scale > 0.1f ? d->scale : 1.0f;

	*w = (rot ? d->h : d->w) / sc;
	*h = (rot ? d->w : d->h) / sc;
}

static void
build_disp(void)
{
	Monitor *m;
	int i, j;

	ndi = 0;
	wl_list_for_each(m, &mons, link) {
		DispInfo *d;
		struct wlr_output_mode *mode;
		RuntimeMonitorConfig *cfg;

		if (!m->wlr_output || !m->wlr_output->enabled)
			continue;
		if (ndi >= CARD_DISP_MAX)
			break;
		d = &di[ndi++];
		memset(d, 0, sizeof(*d));
		d->mon = m;
		snprintf(d->name, sizeof(d->name), "%s", m->wlr_output->name);
		mode = m->wlr_output->current_mode;
		if (mode) {
			d->w = mode->width;
			d->h = mode->height;
			d->hz = mode->refresh / 1000.0f;
		} else {
			d->w = m->wlr_output->width;
			d->h = m->wlr_output->height;
			d->hz = 60.0f;
		}
		d->scale = m->wlr_output->scale > 0.1f ?
			m->wlr_output->scale : 1.0f;
		d->transform = m->wlr_output->transform;
		cfg = monconf_find(d->name);
		if (cfg) {
			d->grid_row = cfg->grid_row > 0 ? cfg->grid_row : 0;
			snprintf(d->mirror, sizeof(d->mirror), "%s",
					cfg->mirror);
		}
	}
	/* left-to-right by layout position */
	for (i = 0; i < ndi; i++)
		for (j = i + 1; j < ndi; j++)
			if (di[j].mon->m.x < di[i].mon->m.x) {
				DispInfo t = di[i];
				di[i] = di[j];
				di[j] = t;
			}
	/* keep the selection pinned to the same output across rebuilds */
	dsel = 0;
	for (i = 0; i < ndi; i++)
		if (dsel_name[0] && strcmp(di[i].name, dsel_name) == 0)
			dsel = i;
	if (ndi > 0)
		snprintf(dsel_name, sizeof(dsel_name), "%s", di[dsel].name);
}

static void
build_rates(const DispInfo *d)
{
	struct wlr_output_mode *mode;
	int i, j;

	di_nrates = 0;
	if (!d->mon || !d->mon->wlr_output)
		return;
	wl_list_for_each(mode, &d->mon->wlr_output->modes, link) {
		float hz = mode->refresh / 1000.0f;

		if (mode->width != d->w || mode->height != d->h)
			continue;
		for (i = 0; i < di_nrates; i++)
			if (fabsf(di_rates[i] - hz) < 0.5f)
				break;
		if (i < di_nrates)
			continue;
		if (di_nrates < DISP_RATES_MAX) {
			di_rates[di_nrates++] = hz;
		} else {
			/* keep the highest rates */
			int lo = 0;
			for (i = 1; i < di_nrates; i++)
				if (di_rates[i] < di_rates[lo])
					lo = i;
			if (hz > di_rates[lo])
				di_rates[lo] = hz;
		}
	}
	for (i = 0; i < di_nrates; i++)
		for (j = i + 1; j < di_nrates; j++)
			if (di_rates[j] > di_rates[i]) {
				float t = di_rates[i];
				di_rates[i] = di_rates[j];
				di_rates[j] = t;
			}
}

/* ── monitors.conf writer ────────────────────────────────────────── */

static void
monconf_write(void)
{
	char tmp[PATH_MAX];
	FILE *fp;
	int i, k;
	int col_in_row[CARD_DISP_MAX] = {0};

	if (!monconf_path_cached[0])
		load_monitors_conf();   /* resolves the path */
	if (!monconf_path_cached[0])
		return;
	if (snprintf(tmp, sizeof(tmp), "%s.tmp", monconf_path_cached) >=
			(int)sizeof(tmp))
		return;
	fp = fopen(tmp, "w");
	if (!fp)
		return;
	fprintf(fp, "# written by nixlytile (Displays popup)\n");
	for (i = 0; i < ndi; i++) {
		DispInfo *d = &di[i];
		int row = d->grid_row;
		int col = row < CARD_DISP_MAX ? col_in_row[row]++ : i;

		fprintf(fp, "monitor = %s grid=%d,%d %dx%d@%.3f",
				d->name, col, row, d->w, d->h, d->hz);
		if (fabsf(d->scale - 1.0f) > 0.01f)
			fprintf(fp, " scale=%.2f", d->scale);
		if (d->transform == WL_OUTPUT_TRANSFORM_90)
			fprintf(fp, " transform=rotate-90");
		else if (d->transform == WL_OUTPUT_TRANSFORM_180)
			fprintf(fp, " transform=rotate-180");
		else if (d->transform == WL_OUTPUT_TRANSFORM_270)
			fprintf(fp, " transform=rotate-270");
		if (d->mirror[0])
			fprintf(fp, " mirror=%s", d->mirror);
		fprintf(fp, "\n");
	}
	/* keep entries for outputs that are not connected right now */
	for (k = 0; k < monconf_monitor_count; k++) {
		RuntimeMonitorConfig *c = &monconf_monitors[k];

		for (i = 0; i < ndi; i++)
			if (strcmp(di[i].name, c->name) == 0)
				break;
		if (i < ndi)
			continue;
		fprintf(fp, "monitor = %s", c->name);
		if (c->grid_col >= 0 && c->grid_row >= 0)
			fprintf(fp, " grid=%d,%d", c->grid_col, c->grid_row);
		if (c->width > 0 && c->height > 0) {
			fprintf(fp, " %dx%d", c->width, c->height);
			if (c->refresh > 0)
				fprintf(fp, "@%.3f", c->refresh);
		}
		if (fabsf(c->scale - 1.0f) > 0.01f)
			fprintf(fp, " scale=%.2f", c->scale);
		if (c->transform == WL_OUTPUT_TRANSFORM_90)
			fprintf(fp, " transform=rotate-90");
		else if (c->transform == WL_OUTPUT_TRANSFORM_180)
			fprintf(fp, " transform=rotate-180");
		else if (c->transform == WL_OUTPUT_TRANSFORM_270)
			fprintf(fp, " transform=rotate-270");
		if (c->mirror[0])
			fprintf(fp, " mirror=%s", c->mirror);
		fprintf(fp, "\n");
	}
	fclose(fp);
	/* rename triggers the IN_MOVED_TO inotify reload in monitors_conf */
	rename(tmp, monconf_path_cached);
}

/* ── icon ────────────────────────────────────────────────────────── */

void
drop_display_icon_buffer(void)
{
	if (display_icon_buf) {
		wlr_buffer_drop(display_icon_buf);
		display_icon_buf = NULL;
	}
	display_icon_loaded_h = 0;
	display_icon_w = display_icon_h = 0;
	display_icon_loaded_path[0] = '\0';
}

int
ensure_display_icon_buffer(int target_h)
{
	GdkPixbuf *pixbuf = NULL;
	GError *gerr = NULL;
	struct wlr_buffer *buf;
	int w = 0, h = 0;
	char resolved[PATH_MAX];
	const char *path = display_icon_path;

	if (target_h <= 0)
		return -1;
	if (resolve_asset_path(display_icon_path, resolved,
				sizeof(resolved)) == 0 && resolved[0])
		path = resolved;
	if (display_icon_buf && display_icon_loaded_h == target_h &&
			strncmp(display_icon_loaded_path, path,
				sizeof(display_icon_loaded_path)) == 0)
		return 0;
	if (tray_load_svg_pixbuf(path, target_h, &pixbuf) != 0) {
		pixbuf = gdk_pixbuf_new_from_file(path, &gerr);
		if (!pixbuf) {
			if (gerr)
				g_error_free(gerr);
			return -1;
		}
	}
	buf = statusbar_buffer_from_pixbuf(pixbuf, target_h, &w, &h);
	if (!buf)
		return -1;
	drop_display_icon_buffer();
	display_icon_buf = buf;
	display_icon_w = w;
	display_icon_h = h;
	display_icon_loaded_h = target_h;
	snprintf(display_icon_loaded_path, sizeof(display_icon_loaded_path),
			"%s", path);
	return 0;
}

void
renderdisplays(StatusModule *module, int bar_height, const char *text)
{
	(void)text;
	render_icon_label(module, bar_height, "",
			ensure_display_icon_buffer, &display_icon_buf,
			&display_icon_w, &display_icon_h, 0,
			statusbar_icon_text_gap, statusbar_fg);
}

/* ── popup ───────────────────────────────────────────────────────── */

static int
btn_hover_idx(int hot, int base, int n)
{
	return hot >= base && hot < base + n ? hot - base : -1;
}

void
render_display_popup(Monitor *m)
{
	InfoPopup *p;
	Card *card;
	CardResult res;
	CardDisp strip[CARD_DISP_MAX];
	char v1[64], v2[64];
	int hot, i;

	if (!m || !m->statusbar.display_popup.tree)
		return;
	p = &m->statusbar.display_popup;
	if (!statusfont.font) {
		p->width = p->height = 0;
		return;
	}

	/* don't rebuild mid-drag: the drag indices point into di[] */
	if (!ddrag.active)
		build_disp();
	if (ndi == 0) {
		p->width = p->height = 0;
		return;
	}
	build_rates(&di[dsel]);

	card = card_begin();
	if (!card)
		return;
	hot = p->btn_hover;

	snprintf(v1, sizeof(v1), "%d", ndi);
	card_header(card, display_icon_path, "Displays", "ARRANGEMENT", v1);
	card_gap(card, 4);

	for (i = 0; i < ndi; i++) {
		float lw, lh;

		disp_logical(&di[i], &lw, &lh);
		snprintf(strip[i].name, sizeof(strip[i].name), "%s",
				di[i].name);
		snprintf(strip[i].sub, sizeof(strip[i].sub), "%dx%d",
				di[i].w, di[i].h);
		strip[i].wr = lw;
		strip[i].hr = lh;
	}
	card_section(card, "LAYOUT · DRAG TO REORDER");
	card_displays(card, strip, ndi, dsel,
			ddrag.active && ddrag.moved ? ddrag.idx : -1,
			ddrag.dx, DHIT_BOX);

	{
		DispInfo *d = &di[dsel];
		static const char *rot_lbl[4] =
			{ "Normal", "90°", "180°", "270°" };

		snprintf(v1, sizeof(v1), "%dx%d @ %.0f Hz", d->w, d->h, d->hz);
		snprintf(v2, sizeof(v2), "%d%%",
				(int)lroundf(d->scale * 100.0f));
		card_kv2(card, "Output", d->name, card_col_blue,
				"Scale", v2, NULL);
		card_kv2(card, "Mode", v1, NULL,
				"Rotation", rot_lbl[transform_index(d->transform)],
				NULL);

		card_section(card, "SCALE");
		{
			const char *lbl[NSCALES];
			static char lblbuf[NSCALES][8];
			int active = -1;

			for (i = 0; i < NSCALES; i++) {
				snprintf(lblbuf[i], sizeof(lblbuf[i]), "%d%%",
					(int)lroundf(scale_presets[i] * 100.0f));
				lbl[i] = lblbuf[i];
				if (fabsf(d->scale - scale_presets[i]) < 0.01f)
					active = i;
			}
			card_buttons(card, lbl, NULL, NSCALES, active,
					btn_hover_idx(hot, DHIT_SCALE, NSCALES),
					DHIT_SCALE);
		}

		if (di_nrates > 1) {
			const char *lbl[DISP_RATES_MAX];
			static char rbuf[DISP_RATES_MAX][12];
			int active = -1;

			card_section(card, "REFRESH RATE");
			for (i = 0; i < di_nrates; i++) {
				snprintf(rbuf[i], sizeof(rbuf[i]), "%.0f Hz",
						di_rates[i]);
				lbl[i] = rbuf[i];
				if (fabsf(d->hz - di_rates[i]) < 0.5f)
					active = i;
			}
			card_buttons(card, lbl, NULL, di_nrates, active,
					btn_hover_idx(hot, DHIT_RATE,
						di_nrates), DHIT_RATE);
		}

		card_section(card, "ROTATION");
		card_buttons(card, rot_lbl, NULL, 4,
				transform_index(d->transform),
				btn_hover_idx(hot, DHIT_ROT, 4), DHIT_ROT);
	}

	if (card_finish(card, &res) != 0)
		return;
	memcpy(p->hits, res.hits, sizeof(p->hits));
	p->nhits = res.nhits;
	popup_view_apply(&p->view, p->tree, &res);
	p->width = p->view.w;
	p->height = p->view.h;
}

/* ── drag to reorder ─────────────────────────────────────────────── */

static CardHit *
find_hit(InfoPopup *p, int id)
{
	int i;

	for (i = 0; i < p->nhits; i++)
		if (p->hits[i].id == id && p->hits[i].w > 0)
			return &p->hits[i];
	return NULL;
}

/* Popup that must stay open while a box drag is in flight (checked by
 * info_popup_hover). */
InfoPopup *
display_drag_popup(void)
{
	if (!ddrag.active || !ddrag.mon)
		return NULL;
	return &ddrag.mon->statusbar.display_popup;
}

void
display_drag_motion(Monitor *m, double cx)
{
	InfoPopup *p;
	CardHit *slot, *nb;
	int nb_idx;

	if (!ddrag.active || ddrag.mon != m)
		return;
	p = &m->statusbar.display_popup;
	ddrag.dx = (int)(cx - ddrag.start_cx);
	if (!ddrag.moved && (ddrag.dx > 4 || ddrag.dx < -4))
		ddrag.moved = 1;
	if (!ddrag.moved)
		return;

	slot = find_hit(p, DHIT_BOX + ddrag.idx);
	if (slot) {
		/* swap with the neighbour once the dragged centre crosses
		 * the neighbour's centre */
		nb_idx = ddrag.dx > 0 ? ddrag.idx + 1 : ddrag.idx - 1;
		nb = nb_idx >= 0 && nb_idx < ndi ?
			find_hit(p, DHIT_BOX + nb_idx) : NULL;
		if (nb) {
			int drag_c = slot->x + ddrag.dx + slot->w / 2;
			int nb_c = nb->x + nb->w / 2;

			if ((ddrag.dx > 0 && drag_c > nb_c) ||
					(ddrag.dx < 0 && drag_c < nb_c)) {
				DispInfo t = di[ddrag.idx];

				di[ddrag.idx] = di[nb_idx];
				di[nb_idx] = t;
				if (dsel == ddrag.idx)
					dsel = nb_idx;
				else if (dsel == nb_idx)
					dsel = ddrag.idx;
				/* keep the box under the cursor: the slot the
				 * drag is measured from just moved */
				ddrag.start_cx += nb->x - slot->x;
				ddrag.dx = (int)(cx - ddrag.start_cx);
				ddrag.idx = nb_idx;
			}
		}
	}
	render_display_popup(m);
}

void
display_drag_release(void)
{
	Monitor *m;

	if (!ddrag.active)
		return;
	m = ddrag.mon;
	ddrag.active = 0;
	if (ddrag.moved)
		monconf_write();
	ddrag.moved = 0;
	ddrag.dx = 0;
	if (m)
		render_display_popup(m);
}

/* ── clicks ──────────────────────────────────────────────────────── */

int
display_popup_handle_click(Monitor *m, int lx, int ly, uint32_t button)
{
	InfoPopup *p = &m->statusbar.display_popup;
	int rel_x, rel_y, i;

	if (!p->visible || p->width <= 0 || p->height <= 0)
		return 0;
	rel_x = lx - p->tree->node.x;
	rel_y = ly - statusbar_popup_y(m);
	if (rel_x < 0 || rel_y < 0 || rel_x >= p->width || rel_y >= p->height)
		return 0;
	if (button != BTN_LEFT)
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

		if (id >= DHIT_BOX && id < DHIT_BOX + ndi) {
			dsel = id - DHIT_BOX;
			snprintf(dsel_name, sizeof(dsel_name), "%s",
					di[dsel].name);
			ddrag.active = 1;
			ddrag.mon = m;
			ddrag.idx = dsel;
			ddrag.start_cx = m->statusbar.area.x + lx;
			ddrag.dx = 0;
			ddrag.moved = 0;
			render_display_popup(m);
		} else if (id >= DHIT_SCALE && id < DHIT_SCALE + NSCALES) {
			di[dsel].scale = scale_presets[id - DHIT_SCALE];
			monconf_write();
			render_display_popup(m);
		} else if (id >= DHIT_RATE && id < DHIT_RATE + di_nrates) {
			di[dsel].hz = di_rates[id - DHIT_RATE];
			monconf_write();
			render_display_popup(m);
		} else if (id >= DHIT_ROT && id < DHIT_ROT + 4) {
			static const int tr[4] = {
				WL_OUTPUT_TRANSFORM_NORMAL,
				WL_OUTPUT_TRANSFORM_90,
				WL_OUTPUT_TRANSFORM_180,
				WL_OUTPUT_TRANSFORM_270,
			};

			di[dsel].transform = tr[id - DHIT_ROT];
			monconf_write();
			render_display_popup(m);
		}
		return 1;
	}
	return 1;
}
