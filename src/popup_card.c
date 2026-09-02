/* Statusbar popup card renderer + show animation. See popup_card.h. */
#include "nixlytile.h"
#include "popup_card.h"

#include <cairo/cairo.h>
#include <librsvg/rsvg.h>
#include <math.h>

#define CARD_PI 3.14159265358979323846

/* ── theme ───────────────────────────────────────────────────────── */

#define CARD_RADIUS   10.0
#define CARD_PAD      16
#define CARD_COLGAP   28
#define CARD_METER_H  44
#define CARD_TICON(h) ((h) + 6)   /* CROW_TEXT row icon size */
#define CARD_BG_R     0.055
#define CARD_BG_G     0.060
#define CARD_BG_B     0.075
#define CARD_BG_A     0.93
#define CARD_BG_A_TILE 0.99   /* backdrop when windows sit under the popup */
#define CARD_BORDER_A 0.09
#define CARD_MIN_W    250

/* show animation */
#define CARD_SHOW_MS     150
#define CARD_SLIDE_PX    10
#define CARD_SWEEP_DELAY 40
#define CARD_SWEEP_MS    300

const float card_col_fg[4]     = {0.92f, 0.93f, 0.95f, 1.0f};
const float card_col_dim[4]    = {0.62f, 0.64f, 0.68f, 1.0f};
const float card_col_faint[4]  = {0.45f, 0.47f, 0.51f, 1.0f};
const float card_col_green[4]  = {0.60f, 0.78f, 0.47f, 1.0f};
const float card_col_yellow[4] = {0.90f, 0.75f, 0.48f, 1.0f};
const float card_col_red[4]    = {1.00f, 0.42f, 0.42f, 1.0f};
const float card_col_blue[4]   = {0.38f, 0.69f, 0.94f, 1.0f};

/* ── fonts ───────────────────────────────────────────────────────── */

static struct fcft_font *card_font_big;
static struct fcft_font *card_font_small;

/* Strip ":size=NN" / ":pixelsize=NN" from a font name so an explicit
 * pixelsize attribute is the only size in the pattern. */
static void
strip_size(const char *name, char *out, size_t len)
{
	size_t o = 0;

	while (*name && o + 1 < len) {
		if (*name == ':' &&
				(!strncmp(name, ":size=", 6) ||
				 !strncmp(name, ":pixelsize=", 11))) {
			name++;
			while (*name && *name != ':')
				name++;
			continue;
		}
		out[o++] = *name++;
	}
	out[o] = '\0';
}

static struct fcft_font *
card_font_load(double factor)
{
	const char **fonts;
	size_t count, i;
	char names[8][256];
	const char *stripped[8];
	char attrs[64];
	int px;

	if (!statusfont.font)
		return NULL;

	if (runtime_fonts_set && runtime_fonts[0]) {
		fonts = (const char **)runtime_fonts;
		for (count = 0; count < 8 && fonts[count]; count++)
			;
	} else {
		fonts = statusbar_fonts;
		for (count = 0; count < 8 && fonts[count]; count++)
			;
	}
	if (count == 0)
		return NULL;

	px = (int)lround((double)statusfont.height * factor);
	if (px < 8)
		px = 8;
	for (i = 0; i < count; i++) {
		strip_size(fonts[i], names[i], sizeof(names[i]));
		stripped[i] = names[i];
	}
	snprintf(attrs, sizeof(attrs), "pixelsize=%d", px);
	return fcft_from_name(count, stripped, attrs);
}

static int
card_fonts_ensure(void)
{
	if (!statusfont.font && !loadstatusfont())
		return 0;
	if (!card_font_big)
		card_font_big = card_font_load(1.30);
	if (!card_font_small)
		card_font_small = card_font_load(0.62);
	return card_font_big && card_font_small;
}

/* ── text measurement / drawing (fcft → pixman) ──────────────────── */

static uint32_t
utf8_next(const char **s)
{
	const unsigned char *p = (const unsigned char *)*s;
	uint32_t cp;
	int len;

	if (p[0] < 0x80) {
		cp = p[0];
		len = 1;
	} else if ((p[0] & 0xE0) == 0xC0) {
		cp = p[0] & 0x1F;
		len = 2;
	} else if ((p[0] & 0xF0) == 0xE0) {
		cp = p[0] & 0x0F;
		len = 3;
	} else if ((p[0] & 0xF8) == 0xF0) {
		cp = p[0] & 0x07;
		len = 4;
	} else {
		(*s)++;
		return 0xFFFD;
	}
	for (int i = 1; i < len; i++) {
		if ((p[i] & 0xC0) != 0x80) {
			len = i;
			break;
		}
		cp = (cp << 6) | (p[i] & 0x3F);
	}
	*s += len;
	return cp;
}

static int
text_width_f(struct fcft_font *f, const char *s, int lspc)
{
	int pen = 0;
	uint32_t prev = 0;

	if (!f || !s)
		return 0;
	while (*s) {
		long kx = 0, ky = 0;
		uint32_t cp = utf8_next(&s);
		const struct fcft_glyph *g;

		if (prev)
			fcft_kerning(f, prev, cp, &kx, &ky);
		pen += (int)kx;
		g = fcft_rasterize_char_utf32(f, cp, FCFT_SUBPIXEL_NONE);
		if (g)
			pen += g->advance.x + (*s ? lspc : 0);
		prev = cp;
	}
	return pen;
}

/* Draw text with its baseline at (x, baseline). Returns advance. */
static int
draw_text_f(pixman_image_t *dst, struct fcft_font *f, const char *s,
		int x, int baseline, const float col[4], int lspc)
{
	int pen = 0;
	uint32_t prev = 0;
	pixman_color_t pc;
	pixman_image_t *solid;

	if (!dst || !f || !s)
		return 0;

	pc.red   = (uint16_t)lroundf(col[0] * 65535.0f);
	pc.green = (uint16_t)lroundf(col[1] * 65535.0f);
	pc.blue  = (uint16_t)lroundf(col[2] * 65535.0f);
	pc.alpha = (uint16_t)lroundf(col[3] * 65535.0f);
	solid = pixman_image_create_solid_fill(&pc);

	while (*s) {
		long kx = 0, ky = 0;
		uint32_t cp = utf8_next(&s);
		const struct fcft_glyph *g;

		if (prev)
			fcft_kerning(f, prev, cp, &kx, &ky);
		pen += (int)kx;
		g = fcft_rasterize_char_utf32(f, cp, FCFT_SUBPIXEL_NONE);
		if (g && g->pix) {
			if (pixman_image_get_format(g->pix) == PIXMAN_a8r8g8b8)
				pixman_image_composite32(PIXMAN_OP_OVER, g->pix,
						NULL, dst, 0, 0, 0, 0,
						x + pen + g->x, baseline - g->y,
						g->width, g->height);
			else
				pixman_image_composite32(PIXMAN_OP_OVER, solid,
						g->pix, dst, 0, 0, 0, 0,
						x + pen + g->x, baseline - g->y,
						g->width, g->height);
		}
		if (g)
			pen += g->advance.x + (*s ? lspc : 0);
		prev = cp;
	}
	if (solid)
		pixman_image_unref(solid);
	return pen;
}

int
card_text_width(const char *s)
{
	if (!statusfont.font)
		return 0;
	return text_width_f(statusfont.font, s, 0);
}

/* ── card build state ────────────────────────────────────────────── */

typedef enum {
	CROW_HEADER,
	CROW_GAUGE,
	CROW_WAVE,
	CROW_LOAD,
	CROW_METER,
	CROW_KV2,
	CROW_SECTION,
	CROW_TEXT,
	CROW_BUTTONS,
	CROW_DISPLAYS,
	CROW_GAP,
	CROW_CAL,
	CROW_CURVE,
	CROW_ICONTEXT,
	CROW_BIGBTN,
	CROW_QR,
} CardRowType;

typedef struct {
	CardRowType type;
	char a[160], b[160], c[96], d[64];
	char micon1[64], micon2[64];   /* CROW_TEXT status icons */
	char btn_label[24];
	int btn_right;
	int btn_solo;       /* btn_right hit rect = button only, no row wash */
	const float *bcol, *dcol;
	double frac;
	float accent[4];
	int nbtn, active, hover, id_base;
	int red_mask;
	char btn[CARD_MAX_BTN][32];
	int gap;
	int hit_id, hot;
	int hit_id2, hot2;   /* CROW_TEXT second right button (label in d) */
	int year, mon, mday;
	CardDisp dsp[CARD_DISP_MAX];
	int ndsp;
	uint8_t cv_t[CARD_CURVE_PTS_MAX], cv_p[CARD_CURVE_PTS_MAX];
	int ncv;
	const uint8_t *qr;   /* CROW_QR module matrix (caller-owned) */
	int qr_size;
	/* computed in measure */
	int y, h;
} CardRow;

struct Card {
	CardRow rows[CARD_MAX_ROWS];
	int nrows;
	int kv_c1, kv_c2;   /* kv2 column widths */
	int kv_k1, kv_k2;   /* kv2 max key widths (values hug keys) */
	int has_kv;
	int min_w;          /* 0 = CARD_MIN_W */
};

Card *
card_begin(void)
{
	Card *c;

	if (!card_fonts_ensure())
		return NULL;
	c = ecalloc(1, sizeof(*c));
	return c;
}

static CardRow *
row_new(Card *c, CardRowType t)
{
	CardRow *r;

	if (!c || c->nrows >= CARD_MAX_ROWS)
		return NULL;
	r = &c->rows[c->nrows++];
	r->type = t;
	r->hit_id = -1;
	r->hit_id2 = -1;
	return r;
}

static void
setstr(char *dst, size_t len, const char *s)
{
	snprintf(dst, len, "%s", s ? s : "");
}

/* key labels get trailing ':' unless empty or already present */
static void
setkey(char *dst, size_t len, const char *s)
{
	size_t n;

	setstr(dst, len, s);
	n = strlen(dst);
	if (n > 0 && dst[n - 1] != ':' && n + 1 < len) {
		dst[n] = ':';
		dst[n + 1] = '\0';
	}
}

void
card_header(Card *c, const char *icon_path, const char *title,
		const char *sub, const char *value)
{
	CardRow *r = row_new(c, CROW_HEADER);

	if (!r)
		return;
	setstr(r->a, sizeof(r->a), icon_path);
	setstr(r->b, sizeof(r->b), title);
	setstr(r->c, sizeof(r->c), sub);
	setstr(r->d, sizeof(r->d), value);
}

void
card_gauge_id(Card *c, double frac, const float accent[4], int hit_id)
{
	CardRow *r = row_new(c, CROW_GAUGE);

	if (!r)
		return;
	if (frac < 0.0)
		frac = 0.0;
	if (frac > 1.0)
		frac = 1.0;
	r->frac = frac;
	r->hit_id = hit_id;
	if (accent)
		memcpy(r->accent, accent, sizeof(r->accent));
	else
		memcpy(r->accent, card_col_fg, sizeof(r->accent));
}

void
card_gauge(Card *c, double frac, const float accent[4])
{
	card_gauge_id(c, frac, accent, -1);
}

void
card_wave(Card *c, double frac, const float accent[4])
{
	CardRow *r = row_new(c, CROW_WAVE);

	if (!r)
		return;
	if (frac < 0.0)
		frac = 0.0;
	if (frac > 1.0)
		frac = 1.0;
	r->frac = frac;
	if (accent)
		memcpy(r->accent, accent, sizeof(r->accent));
	else
		memcpy(r->accent, card_col_fg, sizeof(r->accent));
}

void
card_loading(Card *c, const char *label, double phase)
{
	CardRow *r = row_new(c, CROW_LOAD);

	if (!r)
		return;
	setstr(r->a, sizeof(r->a), label);
	r->frac = phase - floor(phase);
	memcpy(r->accent, card_col_blue, sizeof(r->accent));
}

void
card_meter(Card *c)
{
	row_new(c, CROW_METER);
}

void
card_kv2(Card *c, const char *k1, const char *v1, const float *v1col,
		const char *k2, const char *v2, const float *v2col)
{
	card_kv2_btn(c, k1, v1, v1col, k2, v2, v2col, -1, 0);
}

void
card_kv2_btn(Card *c, const char *k1, const char *v1, const float *v1col,
		const char *k2, const char *v2, const float *v2col,
		int hit_id, int hot)
{
	CardRow *r = row_new(c, CROW_KV2);

	if (!r)
		return;
	setkey(r->a, sizeof(r->a), k1);
	setstr(r->b, sizeof(r->b), v1);
	setkey(r->c, sizeof(r->c), k2);
	setstr(r->d, sizeof(r->d), v2);
	r->bcol = v1col ? v1col : card_col_fg;
	r->dcol = v2col ? v2col : card_col_fg;
	r->hit_id = hit_id;
	r->hot = hot;
}

void
card_section(Card *c, const char *label)
{
	CardRow *r = row_new(c, CROW_SECTION);

	if (!r)
		return;
	setstr(r->a, sizeof(r->a), label);
}

void
card_text(Card *c, const char *left, const char *right, const float *rightcol)
{
	card_text_btn(c, left, right, rightcol, NULL, -1, 0);
}

void
card_text_btn(Card *c, const char *left, const char *right,
		const float *rightcol, const char *btn_label, int hit_id, int hot)
{
	card_icon_text_btn(c, NULL, left, right, rightcol, btn_label,
			hit_id, hot);
}

void
card_icon_text_btn(Card *c, const char *icon_path, const char *left,
		const char *right, const float *rightcol,
		const char *btn_label, int hit_id, int hot)
{
	CardRow *r = row_new(c, CROW_TEXT);

	if (!r)
		return;
	setstr(r->a, sizeof(r->a), left);
	setstr(r->b, sizeof(r->b), right);
	setstr(r->c, sizeof(r->c), icon_path);
	setstr(r->btn_label, sizeof(r->btn_label), btn_label);
	r->bcol = rightcol ? rightcol : card_col_dim;
	r->hit_id = hit_id;
	r->hot = hot;
}

void
card_icon_text_rbtn(Card *c, const char *icon_path, const char *left,
		const char *right, const float *rightcol,
		const char *btn_label, int hit_id, int hot)
{
	card_icon_text_btn(c, icon_path, left, right, rightcol, btn_label,
			hit_id, hot);
	if (c && c->nrows > 0)
		c->rows[c->nrows - 1].btn_right = 1;
}

void
card_icon_text_rbtn_icons(Card *c, const char *icon_path,
		const char *left, const char *sicon1, const char *sicon2,
		const char *btn_label, int hit_id, int hot)
{
	card_icon_text_rbtn(c, icon_path, left, NULL, NULL, btn_label,
			hit_id, hot);
	if (c && c->nrows > 0) {
		CardRow *r = &c->rows[c->nrows - 1];

		setstr(r->micon1, sizeof(r->micon1), sicon1);
		setstr(r->micon2, sizeof(r->micon2), sicon2);
	}
}

void
card_icon_text_hit(Card *c, const char *icon_path, const char *left,
		const char *right, const float *rightcol, const char *sicon,
		int hit_id, int hot)
{
	card_icon_text_btn(c, icon_path, left, right, rightcol, NULL,
			hit_id, hot);
	if (c && c->nrows > 0) {
		CardRow *r = &c->rows[c->nrows - 1];

		r->btn_right = 1;   /* full-row wash + hit, no button */
		setstr(r->micon1, sizeof(r->micon1), sicon);
	}
}

void
card_text_rbtn(Card *c, const char *left, const char *right,
		const float *rightcol, const char *btn_label, int hit_id, int hot)
{
	card_icon_text_btn(c, NULL, left, right, rightcol, btn_label,
			hit_id, hot);
	if (c && c->nrows > 0) {
		c->rows[c->nrows - 1].btn_right = 1;
		c->rows[c->nrows - 1].btn_solo = 1;
	}
}

void
card_text_btn2(Card *c, const char *left,
		const char *btn1, int hit_id1, int hot1,
		const char *btn2, int hit_id2, int hot2)
{
	CardRow *r;

	card_icon_text_btn(c, NULL, left, NULL, NULL, btn1, hit_id1, hot1);
	if (!c || c->nrows == 0)
		return;
	r = &c->rows[c->nrows - 1];
	r->btn_right = 1;
	r->btn_solo = 1;
	setstr(r->d, sizeof(r->d), btn2);
	r->hit_id2 = hit_id2;
	r->hot2 = hot2;
}

void
card_buttons(Card *c, const char *labels[], const char *icons[],
		int n, int active, int hover, int id_base)
{
	(void)icons;
	card_buttons_mask(c, labels, n, active, hover, id_base, 0);
}

void
card_buttons_mask(Card *c, const char *labels[],
		int n, int active, int hover, int id_base, int red_mask)
{
	CardRow *r = row_new(c, CROW_BUTTONS);

	if (!r)
		return;
	if (n > CARD_MAX_BTN)
		n = CARD_MAX_BTN;
	r->nbtn = n;
	r->active = active;
	r->hover = hover;
	r->id_base = id_base;
	r->red_mask = red_mask;
	for (int i = 0; i < n; i++)
		setstr(r->btn[i], sizeof(r->btn[i]), labels[i]);
}

#define DISP_STRIP_H 96
#define CARD_CURVE_H 72   /* curve plot height (labels row below) */

void
card_displays(Card *c, const CardDisp *d, int n, int sel,
		int drag_idx, int drag_dx, int id_base)
{
	CardRow *r = row_new(c, CROW_DISPLAYS);

	if (!r)
		return;
	if (n > CARD_DISP_MAX)
		n = CARD_DISP_MAX;
	memcpy(r->dsp, d, n * sizeof(*d));
	r->ndsp = n;
	r->active = sel;
	r->hover = drag_idx;
	r->gap = drag_dx;
	r->id_base = id_base;
}

void
card_gap(Card *c, int px)
{
	CardRow *r = row_new(c, CROW_GAP);

	if (r)
		r->gap = px;
}

void
card_min_w(Card *c, int w)
{
	if (c && w > 0)
		c->min_w = w;
}

void
card_icon_text(Card *c, const char *icon_path, const char *label,
		const float *labelcol, int hit_id, int hot)
{
	CardRow *r = row_new(c, CROW_ICONTEXT);

	if (!r)
		return;
	setstr(r->a, sizeof(r->a), icon_path);
	setstr(r->b, sizeof(r->b), label);
	r->bcol = labelcol ? labelcol : card_col_fg;
	r->hit_id = hit_id;
	r->hot = hot;
}

void
card_big_btn(Card *c, const char *label, const float accent[4],
		int hit_id, int hot)
{
	CardRow *r = row_new(c, CROW_BIGBTN);

	if (!r)
		return;
	setstr(r->a, sizeof(r->a), label);
	r->hit_id = hit_id;
	r->hot = hot;
	if (accent)
		memcpy(r->accent, accent, sizeof(r->accent));
	else
		memcpy(r->accent, card_col_fg, sizeof(r->accent));
}

void
card_qr(Card *c, const uint8_t *modules, int size)
{
	CardRow *r = row_new(c, CROW_QR);

	if (!r || size <= 0)
		return;
	r->qr = modules;
	r->qr_size = size;
}

void
card_curve(Card *c, const uint8_t *temps, const uint8_t *pcts, int n,
		int sel, const float accent[4], int hit_id)
{
	CardRow *r = row_new(c, CROW_CURVE);

	if (!r)
		return;
	if (n > CARD_CURVE_PTS_MAX)
		n = CARD_CURVE_PTS_MAX;
	memcpy(r->cv_t, temps, (size_t)n);
	memcpy(r->cv_p, pcts, (size_t)n);
	r->ncv = n;
	r->active = sel;
	r->hit_id = hit_id;
	if (accent)
		memcpy(r->accent, accent, sizeof(r->accent));
	else
		memcpy(r->accent, card_col_blue, sizeof(r->accent));
}

void
card_calendar(Card *c, int year, int mon, int mday)
{
	CardRow *r = row_new(c, CROW_CAL);

	if (!r)
		return;
	r->year = year;
	r->mon = mon;
	r->mday = mday;
}

/* ── layout ──────────────────────────────────────────────────────── */

#define SMALL_LSPC 2   /* letterspacing for caps labels */

static int
btn_row_width(CardRow *r)
{
	int w = 0;

	for (int i = 0; i < r->nbtn; i++) {
		w += text_width_f(statusfont.font, r->btn[i], 0) + 2 * 14;
		if (i)
			w += 10;
	}
	return w;
}

static int
cal_cell_w(void)
{
	return text_width_f(card_font_small, "00", SMALL_LSPC) + 10;
}

/* Measure pass: computes each row's height and the card content width. */
static void
card_measure(Card *c, int *out_w, int *out_h)
{
	int base_h = statusfont.height;
	int big_h = card_font_big->height;
	int small_h = card_font_small->height;
	int w = c->min_w > 0 ? c->min_w : CARD_MIN_W, y = CARD_PAD;

	/* kv2 column widths first (shared alignment) */
	c->kv_c1 = c->kv_c2 = 0;
	c->kv_k1 = c->kv_k2 = 0;
	for (int i = 0; i < c->nrows; i++) {
		CardRow *r = &c->rows[i];
		int k1, v1, k2, v2;

		if (r->type != CROW_KV2)
			continue;
		c->has_kv = 1;
		k1 = text_width_f(statusfont.font, r->a, 0);
		v1 = text_width_f(statusfont.font, r->b, 0);
		k2 = r->c[0] ? text_width_f(statusfont.font, r->c, 0) : 0;
		v2 = r->d[0] ? text_width_f(statusfont.font, r->d, 0) : 0;
		if (r->hit_id >= 0)
			v2 += 16;   /* clickable value chip padding */
		if (k1 > c->kv_k1)
			c->kv_k1 = k1;
		if (k2 > c->kv_k2)
			c->kv_k2 = k2;
		if (v1 > c->kv_c1)
			c->kv_c1 = v1;
		if (v2 > c->kv_c2)
			c->kv_c2 = v2;
	}
	/* values left-align after longest key, so col = kmax + gap + vmax */
	if (c->kv_c1 || c->kv_k1)
		c->kv_c1 += c->kv_k1 + 16;
	if (c->kv_c2 || c->kv_k2)
		c->kv_c2 += c->kv_k2 + 16;
	if (c->has_kv) {
		/* equal-width columns so key/value gaps match across rows */
		int kvw = c->kv_c2 ?
			2 * MAX(c->kv_c1, c->kv_c2) + CARD_COLGAP : c->kv_c1;
		if (kvw > w)
			w = kvw;
	}

	for (int i = 0; i < c->nrows; i++) {
		CardRow *r = &c->rows[i];
		int rw = 0;

		r->y = y;
		switch (r->type) {
		case CROW_HEADER: {
			int icon = r->a[0] ? big_h + 12 : 0;
			int left = MAX(text_width_f(statusfont.font, r->b, 0),
					text_width_f(card_font_small, r->c, SMALL_LSPC));
			rw = icon + left + 24 +
				text_width_f(card_font_big, r->d, 0);
			r->h = MAX(base_h + small_h + 4, big_h) + 6;
			break;
		}
		case CROW_GAUGE:
			r->h = 6 + 12;
			break;
		case CROW_WAVE:
			r->h = 26;
			break;
		case CROW_LOAD:
			rw = 26 + text_width_f(statusfont.font, r->a, 0);
			r->h = 34;
			break;
		case CROW_KV2:
			r->h = base_h + (r->hit_id >= 0 ? 10 : 6);
			break;
		case CROW_SECTION:
			r->h = 14 + 1 + (r->a[0] ? 12 + small_h : 0) + 10;
			rw = r->a[0] ?
				text_width_f(card_font_small, r->a, SMALL_LSPC) : 0;
			break;
		case CROW_TEXT:
			rw = text_width_f(statusfont.font, r->a, 0) + 16 +
				text_width_f(statusfont.font, r->b, 0);
			if (r->c[0])
				rw += CARD_TICON(base_h) + 10;
			if (r->micon1[0])
				rw += base_h + 2 + 8;
			if (r->micon2[0])
				rw += base_h + 2 + 8;
			if (r->btn_label[0])
				rw += 12 + text_width_f(statusfont.font,
						r->btn_label, 0) + 16;
			if (r->d[0] && r->hit_id2 >= 0)
				rw += 10 + text_width_f(statusfont.font,
						r->d, 0) + 16;
			r->h = base_h + (r->c[0] ? 10 : 6);
			break;
		case CROW_BUTTONS:
			rw = btn_row_width(r);
			r->h = base_h + 18;
			break;
		case CROW_DISPLAYS:
			rw = r->ndsp * 84;
			r->h = DISP_STRIP_H;
			break;
		case CROW_METER:
			r->h = CARD_METER_H;
			break;
		case CROW_CURVE:
			r->h = CARD_CURVE_H + small_h + 12;
			break;
		case CROW_ICONTEXT:
			rw = base_h + 10 +
				text_width_f(statusfont.font, r->b, 0);
			r->h = base_h + 14;
			break;
		case CROW_BIGBTN:
			rw = text_width_f(statusfont.font, r->a, 0) + 2 * 24;
			r->h = base_h + 26;
			break;
		case CROW_QR: {
			int scale = r->qr_size > 0 ? 200 / r->qr_size : 3;

			if (scale < 3)
				scale = 3;
			if (scale > 6)
				scale = 6;
			r->gap = scale;   /* px per module, reused in draw */
			rw = r->qr_size * scale + 2 * 16;
			r->h = r->qr_size * scale + 2 * 16;
			break;
		}
		case CROW_GAP:
			r->h = r->gap;
			break;
		case CROW_CAL: {
			int cw = cal_cell_w();
			rw = 7 * cw;
			r->h = small_h + 8 + 6 * (small_h + 6);
			break;
		}
		}
		if (rw > w)
			w = rw;
		y += r->h;
	}

	*out_w = w + 2 * CARD_PAD;
	*out_h = y + CARD_PAD;
}

/* ── drawing ─────────────────────────────────────────────────────── */

static void
rounded(cairo_t *cr, double x, double y, double w, double h, double r)
{
	cairo_new_sub_path(cr);
	cairo_arc(cr, x + w - r, y + r, r, -CARD_PI / 2, 0);
	cairo_arc(cr, x + w - r, y + h - r, r, 0, CARD_PI / 2);
	cairo_arc(cr, x + r, y + h - r, r, CARD_PI / 2, CARD_PI);
	cairo_arc(cr, x + r, y + r, r, CARD_PI, 3 * CARD_PI / 2);
	cairo_close_path(cr);
}

/* Icons are re-drawn on every card re-render (popup data refreshes every
 * couple of seconds); cache the rasterized SVG per (path, size) so the
 * disk read + XML parse only happens once. */
#define ICON_CACHE 16

static struct {
	char path[PATH_MAX];
	int size;
	uint64_t used;
	cairo_surface_t *surf;
} icon_cache[ICON_CACHE];
static uint64_t icon_cache_tick;

static void
draw_icon(cairo_t *cr, const char *path, int x, int y, int size)
{
	char resolved[PATH_MAX];
	cairo_surface_t *surf = NULL;
	int lru = 0;

	if (resolve_asset_path(path, resolved, sizeof(resolved)) != 0)
		return;

	for (int i = 0; i < ICON_CACHE; i++) {
		if (icon_cache[i].surf && icon_cache[i].size == size &&
				!strcmp(icon_cache[i].path, resolved)) {
			icon_cache[i].used = ++icon_cache_tick;
			surf = icon_cache[i].surf;
			break;
		}
		if (icon_cache[i].used < icon_cache[lru].used)
			lru = i;
	}

	if (!surf) {
		gchar *data = NULL;
		gsize len = 0;
		RsvgHandle *handle;
		RsvgRectangle vp = { .x = 0, .y = 0,
			.width = size, .height = size };
		cairo_t *icr;

		if (!g_file_get_contents(resolved, &data, &len, NULL) || !data)
			return;
		handle = rsvg_handle_new_from_data((const guint8 *)data, len,
				NULL);
		g_free(data);
		if (!handle)
			return;
		surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
				size, size);
		if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
			cairo_surface_destroy(surf);
			g_object_unref(handle);
			return;
		}
		icr = cairo_create(surf);
		rsvg_handle_render_document(handle, icr, &vp, NULL);
		cairo_destroy(icr);
		g_object_unref(handle);

		if (icon_cache[lru].surf)
			cairo_surface_destroy(icon_cache[lru].surf);
		snprintf(icon_cache[lru].path, sizeof(icon_cache[lru].path),
				"%s", resolved);
		icon_cache[lru].size = size;
		icon_cache[lru].used = ++icon_cache_tick;
		icon_cache[lru].surf = surf;
	}

	cairo_save(cr);
	cairo_set_source_surface(cr, surf, x, y);
	cairo_rectangle(cr, x, y, size, size);
	cairo_fill(cr);
	cairo_restore(cr);
}

/* Shared box layout for a CROW_DISPLAYS strip (used by both the cairo
 * fill pass and the fcft text pass).  Boxes are proportional to the
 * displays' logical sizes, centered in the strip. */
static int
disp_layout(const CardRow *r, int inner_w, int strip_y,
		int *bx, int *by, int *bw, int *bh)
{
	int n = r->ndsp, gap = 12, i, x;
	float sum_wr = 0, max_hr = 0;
	double k, total;

	for (i = 0; i < n; i++) {
		sum_wr += r->dsp[i].wr;
		if (r->dsp[i].hr > max_hr)
			max_hr = r->dsp[i].hr;
	}
	if (n == 0 || sum_wr <= 0.0f || max_hr <= 0.0f)
		return 0;
	k = (double)(inner_w - gap * (n - 1)) / sum_wr;
	if (k * max_hr > DISP_STRIP_H - 16)
		k = (DISP_STRIP_H - 16) / max_hr;
	total = gap * (n - 1);
	for (i = 0; i < n; i++)
		total += r->dsp[i].wr * k;
	x = CARD_PAD + (inner_w - (int)total) / 2;
	if (x < CARD_PAD)
		x = CARD_PAD;
	for (i = 0; i < n; i++) {
		bw[i] = (int)(r->dsp[i].wr * k);
		bh[i] = (int)(r->dsp[i].hr * k);
		if (bw[i] < 40)
			bw[i] = 40;
		if (bh[i] < 40)
			bh[i] = 40;
		bx[i] = x;
		by[i] = strip_y + (DISP_STRIP_H - bh[i]) / 2;
		x += bw[i] + gap;
	}
	return n;
}

static void
add_hit(CardResult *out, int x, int y, int w, int h, int id)
{
	if (out->nhits >= CARD_MAX_HITS)
		return;
	out->hits[out->nhits++] = (CardHit){ .x = x, .y = y, .w = w,
		.h = h, .id = id };
}

/* Popups read fine glassy over the wallpaper, but over window content
 * the bleed-through hurts legibility: when the card actually overlaps
 * a visible client the backdrop goes nearly opaque, over bare
 * wallpaper it stays translucent.  Callers pass their landing spot via
 * card_at(); without it any client visible on selmon darkens (legacy
 * behaviour for toasts/menus that place themselves late). */
static Monitor *card_at_mon;
static int card_at_x, card_at_y, card_at_valid;

void
card_at(struct Monitor *m, int x, int y)
{
	card_at_mon = m;
	card_at_x = x;
	card_at_y = y;
	card_at_valid = m != NULL;
}

static double
card_bg_a(int w, int h)
{
	Client *c;

	if (!card_at_valid) {
		wl_list_for_each(c, &clients, link)
			if (VISIBLEON(c, selmon))
				return CARD_BG_A_TILE;
		return CARD_BG_A;
	}
	wl_list_for_each(c, &clients, link) {
		if (!VISIBLEON(c, card_at_mon))
			continue;
		if (card_at_x < c->geom.x + c->geom.width &&
				card_at_x + w > c->geom.x &&
				card_at_y < c->geom.y + c->geom.height &&
				card_at_y + h > c->geom.y)
			return CARD_BG_A_TILE;
	}
	return CARD_BG_A;
}

int
card_finish(Card *c, CardResult *out)
{
	int w, h, stride;
	cairo_surface_t *cs;
	cairo_t *cr;
	pixman_image_t *pix;
	struct PixmanBuffer *buf;
	void *data;
	int base_h, base_asc, small_h, small_asc, big_asc;
	int inner_w;

	if (!c || !out)
		return -1;
	memset(out, 0, sizeof(*out));
	if (!card_fonts_ensure()) {
		free(c);
		return -1;
	}

	base_h = statusfont.height;
	base_asc = statusfont.ascent;
	small_h = card_font_small->height;
	small_asc = card_font_small->ascent;
	big_asc = card_font_big->ascent;

	card_measure(c, &w, &h);
	inner_w = w - 2 * CARD_PAD;

	cs = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
	if (cairo_surface_status(cs) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(cs);
		card_at_valid = 0;
		free(c);
		return -1;
	}
	cr = cairo_create(cs);

	/* card body + hairline border */
	rounded(cr, 0.5, 0.5, w - 1.0, h - 1.0, CARD_RADIUS);
	cairo_set_source_rgba(cr, CARD_BG_R, CARD_BG_G, CARD_BG_B,
			card_bg_a(w, h));
	card_at_valid = 0;
	cairo_fill_preserve(cr);
	cairo_set_source_rgba(cr, 1, 1, 1, CARD_BORDER_A);
	cairo_set_line_width(cr, 1.0);
	cairo_stroke(cr);

	/* pass A: shapes + icons (cairo) */
	for (int i = 0; i < c->nrows; i++) {
		CardRow *r = &c->rows[i];
		int y = r->y;

		switch (r->type) {
		case CROW_HEADER:
			if (r->a[0]) {
				int isz = card_font_big->height;
				draw_icon(cr, r->a, CARD_PAD,
						y + (r->h - 6 - isz) / 2, isz);
			}
			break;
		case CROW_GAUGE: {
			int gy = y + 3;

			rounded(cr, CARD_PAD, gy, inner_w, 6, 3);
			cairo_set_source_rgba(cr, 1, 1, 1, 0.13);
			cairo_fill(cr);
			if (out->nfills < CARD_MAX_FILLS &&
					(r->frac > 0.0 || r->hit_id >= 0)) {
				int fw = (int)lround(inner_w * r->frac);
				if (fw < 6)
					fw = 6;
				out->fills[out->nfills++] = (CardFill){
					.x = CARD_PAD, .y = gy,
					.w = fw, .h = 6,
					.full_w = inner_w,
					.color = { r->accent[0], r->accent[1],
						r->accent[2], r->accent[3] },
				};
			}
			if (r->hit_id >= 0)
				add_hit(out, CARD_PAD, y, inner_w, r->h,
						r->hit_id);
			break;
		}
		case CROW_WAVE:
			/* faint baseline; the live spectrum is an overlay
			 * buffer the popup drops into this rect each tick
			 * (card_spectrum_buffer) */
			cairo_rectangle(cr, CARD_PAD, y + r->h - 1, inner_w, 1);
			cairo_set_source_rgba(cr, 1, 1, 1, 0.07);
			cairo_fill(cr);
			out->wave_x = CARD_PAD;
			out->wave_y = y;
			out->wave_w = inner_w;
			out->wave_h = r->h;
			break;
		case CROW_LOAD: {
			/* spinner arc + label, centered as one group; the label
			 * glyphs land in pass B at the same offsets */
			int tw = text_width_f(statusfont.font, r->a, 0);
			int gx = (w - (26 + tw)) / 2;
			double cxr = gx + 9, cyr = r->y + r->h / 2.0;
			double a0 = r->frac * 2.0 * M_PI;
			const float *a = r->accent;

			cairo_save(cr);
			cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
			/* faint track ring */
			cairo_arc(cr, cxr, cyr, 7.0, 0, 2.0 * M_PI);
			cairo_set_source_rgba(cr, 1, 1, 1, 0.10);
			cairo_set_line_width(cr, 2.0);
			cairo_stroke(cr);
			/* glowing arc */
			cairo_arc(cr, cxr, cyr, 7.0, a0, a0 + 4.4);
			cairo_set_source_rgba(cr, a[0], a[1], a[2],
					a[3] * 0.25);
			cairo_set_line_width(cr, 4.5);
			cairo_stroke_preserve(cr);
			cairo_set_source_rgba(cr, a[0], a[1], a[2], a[3]);
			cairo_set_line_width(cr, 2.0);
			cairo_stroke(cr);
			cairo_restore(cr);
			break;
		}
		case CROW_METER:
			/* faint midline; the live waveform is an overlay
			 * buffer the popup drops into this rect each tick
			 * (card_meter_buffer) */
			cairo_rectangle(cr, CARD_PAD, y + r->h / 2, inner_w, 1);
			cairo_set_source_rgba(cr, 1, 1, 1, 0.07);
			cairo_fill(cr);
			out->meter_x = CARD_PAD;
			out->meter_y = y;
			out->meter_w = inner_w;
			out->meter_h = r->h;
			break;
		case CROW_KV2:
			/* clickable value chip (e.g. mute toggle) */
			if (r->hit_id >= 0 && r->d[0]) {
				int vw = text_width_f(statusfont.font, r->d, 0);
				int colw = (w - 2 * CARD_PAD - CARD_COLGAP) / 2;
				int bw = vw + 16;
				int bh = base_h + 6;
				int bx = CARD_PAD + colw + CARD_COLGAP +
					c->kv_k2 + 16;
				int by = y + (r->h - bh) / 2;

				rounded(cr, bx, by, bw, bh, 6);
				if (r->hot)
					cairo_set_source_rgba(cr, 1, 1, 1, 0.16);
				else
					cairo_set_source_rgba(cr, 1, 1, 1, 0.07);
				cairo_fill(cr);
				add_hit(out, bx - 4, y, bw + 8, r->h,
						r->hit_id);
			}
			break;
		case CROW_SECTION:
			cairo_rectangle(cr, CARD_PAD, y + 14, inner_w, 1);
			cairo_set_source_rgba(cr, 1, 1, 1, 0.08);
			cairo_fill(cr);
			break;
		case CROW_TEXT:
			/* full-row buttons: faint wash across the row on hover */
			if (r->btn_right && !r->btn_solo && r->hot) {
				rounded(cr, 6, y + 1, w - 12, r->h - 2, 6);
				cairo_set_source_rgba(cr, 1, 1, 1, 0.05);
				cairo_fill(cr);
			}
			if (r->c[0]) {
				int isz = CARD_TICON(base_h);

				draw_icon(cr, r->c, CARD_PAD,
						y + (r->h - isz) / 2, isz);
			}
			if (r->micon1[0] || r->micon2[0]) {
				int isz = base_h + 2;
				int ix = w - CARD_PAD;

				if (r->btn_label[0] && r->hit_id >= 0)
					ix -= text_width_f(statusfont.font,
							r->btn_label, 0) + 16 + 12;
				if (r->micon2[0]) {
					ix -= isz;
					draw_icon(cr, r->micon2, ix,
							y + (r->h - isz) / 2, isz);
					ix -= 8;
				}
				if (r->micon1[0]) {
					ix -= isz;
					draw_icon(cr, r->micon1, ix,
							y + (r->h - isz) / 2, isz);
				}
			}
			/* optional second right-pinned button (text_btn2) */
			if (r->d[0] && r->hit_id2 >= 0) {
				int bw2 = text_width_f(statusfont.font,
						r->d, 0) + 16;
				int bh = base_h + 2;
				int bx2 = w - CARD_PAD - bw2;
				int by = y + (r->h - bh) / 2;

				rounded(cr, bx2, by, bw2, bh, 5);
				if (r->hot2)
					cairo_set_source_rgba(cr, 0.85, 0.30,
							0.30, 0.85);
				else
					cairo_set_source_rgba(cr, 1, 1, 1, 0.10);
				cairo_fill(cr);
				add_hit(out, bx2, y, bw2, r->h, r->hit_id2);
			}
			if (r->btn_label[0] && r->hit_id >= 0) {
				int bw = text_width_f(statusfont.font,
						r->btn_label, 0) + 16;
				int bh = base_h + 2;
				int bx = r->btn_right ? w - CARD_PAD - bw :
					CARD_PAD +
					(r->c[0] ? CARD_TICON(base_h) + 10 : 0) +
					text_width_f(statusfont.font, r->a, 0) +
					12;
				int by = y + (r->h - bh) / 2;

				if (r->d[0] && r->hit_id2 >= 0 && r->btn_right)
					bx -= text_width_f(statusfont.font,
							r->d, 0) + 16 + 10;
				rounded(cr, bx, by, bw, bh, 5);
				if (r->hot)
					cairo_set_source_rgba(cr, 0.85, 0.30,
							0.30, 0.85);
				else
					cairo_set_source_rgba(cr, 1, 1, 1, 0.10);
				cairo_fill(cr);
				/* right-pinned buttons act on the whole row
				 * (solo: button rect only — Kill rows) */
				if (r->btn_right && !r->btn_solo)
					add_hit(out, 0, y, w, r->h,
							r->hit_id);
				else
					add_hit(out, bx, y, bw, r->h,
							r->hit_id);
			} else if (r->btn_right && r->hit_id >= 0) {
				/* buttonless full-row hit (card_icon_text_hit) */
				add_hit(out, 0, y, w, r->h, r->hit_id);
			}
			break;
		case CROW_BUTTONS: {
			int n = r->nbtn;
			int gap = 10;
			int bw = n > 0 ? (inner_w - (n - 1) * gap) / n : 0;
			int bh = r->h - 4;

			for (int b = 0; b < n; b++) {
				int bx = CARD_PAD + b * (bw + gap);
				int red = r->red_mask >> b & 1;

				rounded(cr, bx + 0.5, y + 0.5, bw - 1, bh - 1, 8);
				if (b == r->active) {
					if (red)
						cairo_set_source_rgba(cr,
								0.85, 0.30, 0.30, 0.30);
					else
						cairo_set_source_rgba(cr, 1, 1, 1, 0.14);
					cairo_fill_preserve(cr);
					if (red)
						cairo_set_source_rgba(cr,
								0.90, 0.35, 0.35, 0.60);
					else
						cairo_set_source_rgba(cr, 1, 1, 1, 0.25);
				} else if (b == r->hover) {
					if (red)
						cairo_set_source_rgba(cr,
								0.85, 0.30, 0.30, 0.18);
					else
						cairo_set_source_rgba(cr, 1, 1, 1, 0.07);
					cairo_fill_preserve(cr);
					if (red)
						cairo_set_source_rgba(cr,
								0.90, 0.35, 0.35, 0.45);
					else
						cairo_set_source_rgba(cr, 1, 1, 1, 0.14);
				} else if (red) {
					cairo_set_source_rgba(cr,
							0.90, 0.35, 0.35, 0.35);
				} else {
					cairo_set_source_rgba(cr, 1, 1, 1, 0.13);
				}
				cairo_set_line_width(cr, 1.0);
				cairo_stroke(cr);
				add_hit(out, bx, y, bw, bh, r->id_base + b);
			}
			break;
		}
		case CROW_BIGBTN: {
			const float *a = r->accent;

			rounded(cr, CARD_PAD + 0.5, y + 2.5, inner_w - 1,
					r->h - 5, 8);
			cairo_set_source_rgba(cr, a[0], a[1], a[2],
					r->hot ? 0.38 : 0.22);
			cairo_fill_preserve(cr);
			cairo_set_source_rgba(cr, a[0], a[1], a[2],
					r->hot ? 0.95 : 0.60);
			cairo_set_line_width(cr, 1.0);
			cairo_stroke(cr);
			if (r->hit_id >= 0)
				add_hit(out, CARD_PAD, y, inner_w, r->h,
						r->hit_id);
			break;
		}
		case CROW_QR: {
			int scale = r->gap;
			int qs = r->qr_size * scale;
			int qx = CARD_PAD + (inner_w - qs - 2 * 16) / 2;
			int mx, my;

			/* white plate = quiet zone; QR needs the contrast,
			 * the translucent card bg is not enough */
			rounded(cr, qx, y, qs + 2 * 16, qs + 2 * 16, 8);
			cairo_set_source_rgba(cr, 1, 1, 1, 1);
			cairo_fill(cr);
			cairo_set_source_rgba(cr, 0, 0, 0, 1);
			for (my = 0; my < r->qr_size; my++)
				for (mx = 0; mx < r->qr_size; mx++)
					if (r->qr[my * r->qr_size + mx] & 1)
						cairo_rectangle(cr,
								qx + 16 + mx * scale,
								y + 16 + my * scale,
								scale, scale);
			cairo_fill(cr);
			break;
		}
		case CROW_ICONTEXT: {
			int isz = base_h;

			if (r->hot) {
				rounded(cr, CARD_PAD - 6, y + 1,
						inner_w + 12, r->h - 2, 7);
				cairo_set_source_rgba(cr, 1, 1, 1, 0.10);
				cairo_fill(cr);
			}
			if (r->a[0])
				draw_icon(cr, r->a, CARD_PAD,
						y + (r->h - isz) / 2, isz);
			if (r->hit_id >= 0)
				add_hit(out, CARD_PAD - 6, y, inner_w + 12,
						r->h, r->hit_id);
			break;
		}
		case CROW_CURVE: {
			const float *a = r->accent;
			double span = CARD_CURVE_TMAX - CARD_CURVE_TMIN;
			double px[CARD_CURVE_PTS_MAX], py[CARD_CURVE_PTS_MAX];

			/* plot backdrop + quarter gridlines */
			rounded(cr, CARD_PAD, y, inner_w, CARD_CURVE_H, 6);
			cairo_set_source_rgba(cr, 1, 1, 1, 0.05);
			cairo_fill(cr);
			for (int g = 1; g <= 3; g++) {
				cairo_rectangle(cr, CARD_PAD,
						y + g * CARD_CURVE_H / 4.0,
						inner_w, 1);
				cairo_set_source_rgba(cr, 1, 1, 1, 0.06);
				cairo_fill(cr);
			}

			for (int p = 0; p < r->ncv; p++) {
				double t = r->cv_t[p];

				if (t < CARD_CURVE_TMIN)
					t = CARD_CURVE_TMIN;
				if (t > CARD_CURVE_TMAX)
					t = CARD_CURVE_TMAX;
				px[p] = CARD_PAD + (t - CARD_CURVE_TMIN) /
						span * inner_w;
				py[p] = y + CARD_CURVE_H -
						r->cv_p[p] / 100.0 * CARD_CURVE_H;
			}
			if (r->ncv > 0) {
				/* step outline: pct[i] holds from temp[i] up
				 * to the next threshold; pct[0] floors the
				 * left edge */
				cairo_save(cr);
				rounded(cr, CARD_PAD, y, inner_w, CARD_CURVE_H, 6);
				cairo_clip(cr);
				cairo_move_to(cr, CARD_PAD, py[0]);
				cairo_line_to(cr, px[0], py[0]);
				for (int p = 1; p < r->ncv; p++) {
					cairo_line_to(cr, px[p], py[p - 1]);
					cairo_line_to(cr, px[p], py[p]);
				}
				cairo_line_to(cr, CARD_PAD + inner_w,
						py[r->ncv - 1]);
				cairo_set_source_rgba(cr, a[0], a[1], a[2],
						a[3] * 0.9);
				cairo_set_line_width(cr, 2.0);
				cairo_stroke_preserve(cr);
				/* soft fill down to the floor */
				cairo_line_to(cr, CARD_PAD + inner_w,
						y + CARD_CURVE_H);
				cairo_line_to(cr, CARD_PAD, y + CARD_CURVE_H);
				cairo_close_path(cr);
				cairo_set_source_rgba(cr, a[0], a[1], a[2],
						a[3] * 0.12);
				cairo_fill(cr);
				cairo_restore(cr);
			}
			for (int p = 0; p < r->ncv; p++) {
				if (p == r->active) {
					cairo_arc(cr, px[p], py[p], 5.5,
							0, 2 * CARD_PI);
					cairo_set_source_rgba(cr, 1, 1, 1, 0.9);
					cairo_set_line_width(cr, 1.5);
					cairo_stroke(cr);
				}
				cairo_arc(cr, px[p], py[p],
						p == r->active ? 4.0 : 3.2,
						0, 2 * CARD_PI);
				cairo_set_source_rgba(cr, a[0], a[1], a[2],
						a[3]);
				cairo_fill(cr);
			}
			if (r->hit_id >= 0)
				add_hit(out, CARD_PAD, y, inner_w,
						CARD_CURVE_H, r->hit_id);
			break;
		}
		case CROW_DISPLAYS: {
			int bx[CARD_DISP_MAX], by[CARD_DISP_MAX];
			int bw[CARD_DISP_MAX], bh[CARD_DISP_MAX];
			int n = disp_layout(r, inner_w, y, bx, by, bw, bh);
			int pass, b;

			/* dragged box drawn last so it floats above the rest */
			for (pass = 0; pass < 2; pass++)
				for (b = 0; b < n; b++) {
					int dragged = b == r->hover;
					int x0 = bx[b];

					if (dragged != pass)
						continue;
					if (dragged) {
						x0 += r->gap;
						if (x0 < CARD_PAD)
							x0 = CARD_PAD;
						if (x0 + bw[b] >
								CARD_PAD + inner_w)
							x0 = CARD_PAD +
								inner_w - bw[b];
					}
					rounded(cr, x0 + 0.5, by[b] + 0.5,
							bw[b] - 1, bh[b] - 1, 7);
					cairo_set_source_rgba(cr, 1, 1, 1,
							dragged ? 0.16 :
							b == r->active ?
							0.11 : 0.05);
					cairo_fill_preserve(cr);
					if (b == r->active)
						cairo_set_source_rgba(cr,
							card_col_blue[0],
							card_col_blue[1],
							card_col_blue[2],
							0.90);
					else
						cairo_set_source_rgba(cr,
							1, 1, 1, 0.14);
					cairo_set_line_width(cr,
							b == r->active ?
							1.6 : 1.0);
					cairo_stroke(cr);
					add_hit(out, bx[b], y, bw[b],
							r->h, r->id_base + b);
				}
			break;
		}
		case CROW_CAL: {
			/* today pill */
			struct tm tmv = {0};
			int cw = cal_cell_w(), ch = small_h + 6;
			int wday0, col, rowi;

			tmv.tm_year = r->year - 1900;
			tmv.tm_mon = r->mon;
			tmv.tm_mday = 1;
			tmv.tm_hour = 12;
			if (mktime(&tmv) != (time_t)-1) {
				wday0 = (tmv.tm_wday + 6) % 7; /* Mon=0 */
				col = (wday0 + r->mday - 1) % 7;
				rowi = (wday0 + r->mday - 1) / 7;
				rounded(cr, CARD_PAD + col * cw - 3,
						y + small_h + 8 + rowi * ch - 2,
						cw - 4 + 6, ch - 2, 6);
				cairo_set_source_rgba(cr, card_col_blue[0],
						card_col_blue[1],
						card_col_blue[2], 0.30);
				cairo_fill(cr);
			}
			break;
		}
		default:
			break;
		}
	}

	cairo_destroy(cr);
	cairo_surface_flush(cs);

	/* copy pixels out of the cairo surface into our own allocation */
	stride = cairo_image_surface_get_stride(cs);
	data = ecalloc(1, (size_t)stride * (size_t)h);
	memcpy(data, cairo_image_surface_get_data(cs),
			(size_t)stride * (size_t)h);
	cairo_surface_destroy(cs);

	pix = pixman_image_create_bits(PIXMAN_a8r8g8b8, w, h, data, stride);
	if (!pix) {
		free(data);
		free(c);
		return -1;
	}

	/* pass B: text (fcft glyphs via pixman) */
	for (int i = 0; i < c->nrows; i++) {
		CardRow *r = &c->rows[i];
		int y = r->y;

		switch (r->type) {
		case CROW_HEADER: {
			int tx = CARD_PAD +
				(r->a[0] ? card_font_big->height + 12 : 0);
			int content_h = r->h - 6;
			int block = base_h + (r->c[0] ? small_h + 4 : 0);
			int ty = y + (content_h - block) / 2;

			draw_text_f(pix, statusfont.font, r->b, tx,
					ty + base_asc, card_col_fg, 0);
			if (r->c[0])
				draw_text_f(pix, card_font_small, r->c, tx,
						ty + base_h + 4 + small_asc,
						card_col_faint, SMALL_LSPC);
			if (r->d[0]) {
				int vw = text_width_f(card_font_big, r->d, 0);
				int vy = y + (content_h - card_font_big->height) / 2;

				draw_text_f(pix, card_font_big, r->d,
						w - CARD_PAD - vw,
						vy + big_asc, card_col_fg, 0);
			}
			break;
		}
		case CROW_KV2: {
			/* center within the row (chip rows are taller) */
			int bl = y + (r->h - base_h) / 2 + base_asc;
			int colw = (w - 2 * CARD_PAD - CARD_COLGAP) / 2;
			int x2 = CARD_PAD + colw + CARD_COLGAP;

			draw_text_f(pix, statusfont.font, r->a, CARD_PAD, bl,
					card_col_dim, 0);
			if (r->b[0])
				draw_text_f(pix, statusfont.font, r->b,
						CARD_PAD + c->kv_k1 + 16, bl,
						r->bcol, 0);
			if (r->c[0] || r->d[0]) {
				int vx = x2 + c->kv_k2 + 16;

				if (r->c[0])
					draw_text_f(pix, statusfont.font, r->c,
							x2, bl, card_col_dim, 0);
				if (r->d[0])
					draw_text_f(pix, statusfont.font, r->d,
							r->hit_id >= 0 ?
							vx + 8 : vx, bl,
							r->dcol, 0);
			}
			break;
		}
		case CROW_SECTION:
			if (r->a[0])
				draw_text_f(pix, card_font_small, r->a,
						CARD_PAD, y + 14 + 1 + 12 +
						small_asc, card_col_faint,
						SMALL_LSPC);
			break;
		case CROW_LOAD:
			if (r->a[0]) {
				int tw = text_width_f(statusfont.font, r->a, 0);
				int gx = (w - (26 + tw)) / 2;

				draw_text_f(pix, statusfont.font, r->a, gx + 26,
						y + (r->h - base_h) / 2 + base_asc,
						card_col_dim, 0);
			}
			break;
		case CROW_TEXT: {
			int bl = y + (r->h - base_h) / 2 + base_asc;
			int tx = CARD_PAD + (r->c[0] ? CARD_TICON(base_h) + 10 : 0);
			int bw = 0;

			if (r->btn_label[0] && r->hit_id >= 0)
				bw = text_width_f(statusfont.font,
						r->btn_label, 0) + 16;
			draw_text_f(pix, statusfont.font, r->a, tx, bl,
					card_col_fg, 0);
			if (r->b[0]) {
				int vw = text_width_f(statusfont.font, r->b, 0);
				int vx = w - CARD_PAD - vw;

				if (r->btn_right && bw)
					vx -= bw + 12;
				/* sit left of the right-edge status icons */
				if (r->micon1[0])
					vx -= base_h + 2 + 8;
				if (r->micon2[0])
					vx -= base_h + 2 + 8;
				draw_text_f(pix, statusfont.font, r->b, vx, bl,
						r->bcol, 0);
			}
			if (r->d[0] && r->hit_id2 >= 0) {
				int bw2 = text_width_f(statusfont.font,
						r->d, 0) + 16;
				int bh = base_h + 2;
				int by = y + (r->h - bh) / 2;

				draw_text_f(pix, statusfont.font, r->d,
						w - CARD_PAD - bw2 + 8,
						by + 1 + base_asc,
						card_col_fg, 0);
			}
			if (bw) {
				int bx = r->btn_right ? w - CARD_PAD - bw :
					tx + text_width_f(statusfont.font,
							r->a, 0) + 12;
				int bh = base_h + 2;
				int by = y + (r->h - bh) / 2;

				if (r->d[0] && r->hit_id2 >= 0 && r->btn_right)
					bx -= text_width_f(statusfont.font,
							r->d, 0) + 16 + 10;
				draw_text_f(pix, statusfont.font, r->btn_label,
						bx + 8, by + 1 + base_asc,
						card_col_fg, 0);
			}
			break;
		}
		case CROW_BUTTONS: {
			int n = r->nbtn;
			int gap = 10;
			int bw = n > 0 ? (inner_w - (n - 1) * gap) / n : 0;
			int bh = r->h - 4;

			for (int b = 0; b < n; b++) {
				int bx = CARD_PAD + b * (bw + gap);
				int tw = text_width_f(statusfont.font,
						r->btn[b], 0);
				int bl = y + (bh - base_h) / 2 + base_asc;

				draw_text_f(pix, statusfont.font, r->btn[b],
						bx + (bw - tw) / 2, bl,
						r->red_mask >> b & 1 ?
						card_col_red :
						(b == r->active ? card_col_fg :
						card_col_dim), 0);
			}
			break;
		}
		case CROW_ICONTEXT:
			draw_text_f(pix, statusfont.font, r->b,
					CARD_PAD + base_h + 10,
					y + (r->h - base_h) / 2 + base_asc,
					r->bcol, 0);
			break;
		case CROW_BIGBTN: {
			int tw = text_width_f(statusfont.font, r->a, 0);

			draw_text_f(pix, statusfont.font, r->a,
					CARD_PAD + (inner_w - tw) / 2,
					y + (r->h - base_h) / 2 + base_asc,
					card_col_fg, 0);
			break;
		}
		case CROW_CURVE: {
			double span = CARD_CURVE_TMAX - CARD_CURVE_TMIN;
			int ly = y + CARD_CURVE_H + 6 + small_asc;

			for (int p = 0; p < r->ncv; p++) {
				char t[8];
				double t_cl = r->cv_t[p];
				int tw, tx;

				if (t_cl < CARD_CURVE_TMIN)
					t_cl = CARD_CURVE_TMIN;
				if (t_cl > CARD_CURVE_TMAX)
					t_cl = CARD_CURVE_TMAX;
				snprintf(t, sizeof(t), "%d", r->cv_t[p]);
				tw = text_width_f(card_font_small, t,
						SMALL_LSPC);
				tx = CARD_PAD + (int)((t_cl - CARD_CURVE_TMIN) /
						span * inner_w) - tw / 2;
				if (tx < CARD_PAD)
					tx = CARD_PAD;
				if (tx + tw > w - CARD_PAD)
					tx = w - CARD_PAD - tw;
				draw_text_f(pix, card_font_small, t, tx, ly,
						p == r->active ? card_col_fg :
						card_col_faint, SMALL_LSPC);
			}
			if (r->active >= 0 && r->active < r->ncv) {
				char lab[24];
				int tw;

				snprintf(lab, sizeof(lab), "%d\302\260C \302\267 %d%%",
						r->cv_t[r->active],
						r->cv_p[r->active]);
				tw = text_width_f(card_font_small, lab,
						SMALL_LSPC);
				draw_text_f(pix, card_font_small, lab,
						w - CARD_PAD - tw - 8,
						y + 6 + small_asc,
						card_col_fg, SMALL_LSPC);
			}
			break;
		}
		case CROW_DISPLAYS: {
			int bx[CARD_DISP_MAX], by[CARD_DISP_MAX];
			int bw[CARD_DISP_MAX], bh[CARD_DISP_MAX];
			int n = disp_layout(r, inner_w, y, bx, by, bw, bh);
			int b;

			for (b = 0; b < n; b++) {
				struct fcft_font *nf = statusfont.font;
				int x0 = bx[b], tw, block, ty;

				if (b == r->hover) {
					x0 += r->gap;
					if (x0 < CARD_PAD)
						x0 = CARD_PAD;
					if (x0 + bw[b] > CARD_PAD + inner_w)
						x0 = CARD_PAD + inner_w - bw[b];
				}
				if (text_width_f(nf, r->dsp[b].name, 0) >
						bw[b] - 10)
					nf = card_font_small;
				block = nf->height + 3 + small_h;
				ty = by[b] + (bh[b] - block) / 2;
				tw = text_width_f(nf, r->dsp[b].name, 0);
				draw_text_f(pix, nf, r->dsp[b].name,
						x0 + (bw[b] - tw) / 2,
						ty + nf->ascent,
						b == r->active ? card_col_fg :
						card_col_dim, 0);
				tw = text_width_f(card_font_small,
						r->dsp[b].sub, 0);
				draw_text_f(pix, card_font_small,
						r->dsp[b].sub,
						x0 + (bw[b] - tw) / 2,
						ty + nf->height + 3 + small_asc,
						card_col_faint, 0);
			}
			break;
		}
		case CROW_CAL: {
			static const char *dows[7] =
				{ "Mo", "Tu", "We", "Th", "Fr", "Sa", "Su" };
			struct tm tmv = {0};
			int cw = cal_cell_w(), ch = small_h + 6;
			int wday0, days;

			for (int d = 0; d < 7; d++)
				draw_text_f(pix, card_font_small, dows[d],
						CARD_PAD + d * cw,
						y + small_asc, card_col_faint,
						SMALL_LSPC);

			tmv.tm_year = r->year - 1900;
			tmv.tm_mon = r->mon;
			tmv.tm_mday = 1;
			tmv.tm_hour = 12;
			if (mktime(&tmv) == (time_t)-1)
				break;
			wday0 = (tmv.tm_wday + 6) % 7;
			{
				static const int dim[12] = { 31, 28, 31, 30, 31,
					30, 31, 31, 30, 31, 30, 31 };
				int yy = r->year;
				days = dim[r->mon];
				if (r->mon == 1 && ((yy % 4 == 0 && yy % 100 != 0)
						|| yy % 400 == 0))
					days = 29;
			}
			for (int d = 1; d <= days; d++) {
				int cell = wday0 + d - 1;
				int col = cell % 7, rowi = cell / 7;
				char num[4];

				snprintf(num, sizeof(num), "%d", d);
				draw_text_f(pix, card_font_small, num,
						CARD_PAD + col * cw,
						y + small_h + 8 + rowi * ch +
						small_asc,
						d == r->mday ? card_col_fg :
						card_col_dim, 0);
			}
			break;
		}
		default:
			break;
		}
	}

	buf = ecalloc(1, sizeof(*buf));
	buf->image = pix;
	buf->data = data;
	buf->drm_format = DRM_FORMAT_ARGB8888;
	buf->stride = stride;
	buf->owns_data = 1;
	wlr_buffer_init(&buf->base, &pixman_buffer_impl, w, h);

	out->buf = &buf->base;
	out->w = w;
	out->h = h;
	free(c);
	return 0;
}

/* ── cairo surface → wlr_buffer ──────────────────────────────────── */

/* Copy a finished cairo surface into a PixmanBuffer-backed wlr_buffer
 * and destroy the surface. */
static struct wlr_buffer *
cairo_buf_finish(cairo_surface_t *cs)
{
	struct PixmanBuffer *buf;
	void *data;
	int stride, w, h;

	cairo_surface_flush(cs);
	w = cairo_image_surface_get_width(cs);
	h = cairo_image_surface_get_height(cs);
	stride = cairo_image_surface_get_stride(cs);
	data = ecalloc(1, (size_t)stride * (size_t)h);
	memcpy(data, cairo_image_surface_get_data(cs),
			(size_t)stride * (size_t)h);
	cairo_surface_destroy(cs);

	buf = ecalloc(1, sizeof(*buf));
	buf->image = pixman_image_create_bits(PIXMAN_a8r8g8b8, w, h,
			data, stride);
	buf->data = data;
	buf->drm_format = DRM_FORMAT_ARGB8888;
	buf->stride = stride;
	buf->owns_data = 1;
	wlr_buffer_init(&buf->base, &pixman_buffer_impl, w, h);
	return &buf->base;
}

static cairo_t *
cairo_buf_begin(int w, int h, cairo_surface_t **out_cs)
{
	cairo_surface_t *cs;

	cs = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
	if (cairo_surface_status(cs) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(cs);
		return NULL;
	}
	*out_cs = cs;
	return cairo_create(cs);
}

/* ── gauge fill buffer ───────────────────────────────────────────── */

static struct wlr_buffer *
make_fill_buffer(int w, int h, const float col[4])
{
	cairo_surface_t *cs;
	cairo_t *cr = cairo_buf_begin(w, h, &cs);

	if (!cr)
		return NULL;
	rounded(cr, 0, 0, w, h, h / 2.0);
	cairo_set_source_rgba(cr, col[0], col[1], col[2], col[3]);
	cairo_fill(cr);
	cairo_destroy(cr);
	return cairo_buf_finish(cs);
}

/* ── shared card chrome (tray menus) ─────────────────────────────── */

/* Live audio meter: mirrored rounded bars around the midline, newest at
 * the right. hist is a ring of peak levels [0,1]; head is the newest
 * index. Bars brighten with amplitude so silence stays near-invisible
 * against the card. phase is the fraction [0,1] of the sample period
 * elapsed since head was pushed: bars slide left continuously so the
 * scroll stays smooth between pushes, the newest bar entering from the
 * right edge. */
static void
card_meter_draw(cairo_t *cr, int w, int h, const float accent[4],
		const float *hist, int nhist, int head, double phase)
{
	const int bar_w = 3, gap = 3;
	int nbars = (w + gap) / (bar_w + gap);
	double mid = h / 2.0;
	double slide;

	if (phase < 0.0)
		phase = 0.0;
	if (phase > 2.0)
		phase = 2.0;
	slide = (1.0 - phase) * (bar_w + gap);
	/* extra bars so the left edge stays filled mid-slide (two when a
	 * late push lets the phase run past 1.0) */
	for (int i = 0; i <= nbars + 1; i++) {
		float v = hist[((head - i) % nhist + nhist) % nhist];
		double amp, bh;
		double x = w - bar_w - i * (bar_w + gap) + slide;

		if (v < 0.0f)
			v = 0.0f;
		if (v > 1.0f)
			v = 1.0f;
		amp = sqrt((double)v);   /* perceptual boost for low levels */
		bh = 2.0 + amp * (h - 6);
		rounded(cr, x, mid - bh / 2.0, bar_w, bh, bar_w / 2.0);
		cairo_set_source_rgba(cr, accent[0], accent[1], accent[2],
				0.28 + 0.72 * amp);
		cairo_fill(cr);
	}
}

struct wlr_buffer *
card_meter_buffer(int w, int h, const float accent[4],
		const float *hist, int nhist, int head, double phase)
{
	cairo_surface_t *cs;
	cairo_t *cr = cairo_buf_begin(w, h, &cs);

	if (!cr)
		return NULL;
	card_meter_draw(cr, w, h, accent, hist, nhist, head, phase);
	cairo_destroy(cr);
	return cairo_buf_finish(cs);
}

void
card_meter_raster_finish(MeterRaster *mr)
{
	int i;

	for (i = 0; i < 2; i++) {
		if (mr->buf[i])
			wlr_buffer_drop(mr->buf[i]);
		mr->buf[i] = NULL;
	}
	mr->next = 0;
	mr->w = mr->h = 0;
}

/* Per-frame meter path: rendered every displayed frame, so the generic
 * cairo_buf_begin/finish route (surface alloc + zero, ecalloc, full
 * memcpy, buffer freed right after the scene takes it) was pure
 * per-frame churn.  Draw with cairo directly over a persistent pixman
 * buffer's pixels instead; two buffers alternate because the scene may
 * still hold the previous frame's. */
struct wlr_buffer *
card_meter_raster(MeterRaster *mr, int w, int h, const float accent[4],
		const float *hist, int nhist, int head, double phase)
{
	struct PixmanBuffer *buf;
	cairo_surface_t *cs;
	cairo_t *cr;
	struct wlr_buffer *out;

	if (w <= 0 || h <= 0)
		return NULL;
	if (mr->w != w || mr->h != h) {
		card_meter_raster_finish(mr);
		mr->w = w;
		mr->h = h;
	}
	if (!mr->buf[mr->next]) {
		int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, w);
		buf = ecalloc(1, sizeof(*buf));
		buf->data = ecalloc(1, (size_t)stride * (size_t)h);
		buf->image = pixman_image_create_bits(PIXMAN_a8r8g8b8, w, h,
				buf->data, stride);
		buf->drm_format = DRM_FORMAT_ARGB8888;
		buf->stride = stride;
		buf->owns_data = 1;
		wlr_buffer_init(&buf->base, &pixman_buffer_impl, w, h);
		mr->buf[mr->next] = &buf->base;
	} else {
		buf = wl_container_of(mr->buf[mr->next], buf, base);
	}

	cs = cairo_image_surface_create_for_data(buf->data,
			CAIRO_FORMAT_ARGB32, w, h, buf->stride);
	if (cairo_surface_status(cs) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(cs);
		return NULL;
	}
	cr = cairo_create(cs);
	/* reused pixels — clear the previous frame's bars */
	cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
	cairo_paint(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
	card_meter_draw(cr, w, h, accent, hist, nhist, head, phase);
	cairo_destroy(cr);
	cairo_surface_flush(cs);
	cairo_surface_destroy(cs);

	out = mr->buf[mr->next];
	mr->next ^= 1;
	return out;
}

/* Per-bar spectrum level: two incommensurate sines beat against each
 * other so the motion reads organic, never a marching wave. */
static double
spec_level(double ph, int i)
{
	double v = 0.5 + 0.30 * sin(ph * 2.1 + i * 0.83) +
			0.24 * sin(ph * 3.7 + i * 1.94 + 1.7);

	return v < 0.0 ? 0.0 : v > 1.0 ? 1.0 : v;
}

/* Wi-Fi spectrum analyzer: bottom-aligned bars tapering toward the
 * high end, with peak-hold caps. Overall height and motion speed scale
 * with frac (signal strength) so a weak link idles and a strong one is
 * lively. t is wall time in seconds; each frame is a pure function of
 * (t, frac) so the overlay needs no history state. */
struct wlr_buffer *
card_spectrum_buffer(int w, int h, const float accent[4], double frac,
		double t)
{
	cairo_surface_t *cs;
	cairo_t *cr = cairo_buf_begin(w, h, &cs);
	const int bar_w = 3, gap = 2;
	int nbars = (w + gap) / (bar_w + gap);
	double base = h - 1.0;

	if (!cr)
		return NULL;
	if (frac < 0.0)
		frac = 0.0;
	if (frac > 1.0)
		frac = 1.0;
	for (int i = 0; i < nbars; i++) {
		double ph = t * (1.2 + 1.0 * frac);
		double tilt = 1.0 - 0.45 * i / (nbars > 1 ? nbars - 1 : 1);
		double env = (0.2 + 0.8 * frac) * tilt * (h - 5.0);
		double v = spec_level(ph, i);
		double x = i * (bar_w + gap);
		double bh = 2.0 + env * v;
		double peak = v;

		rounded(cr, x, base - bh, bar_w, bh, 1.5);
		cairo_set_source_rgba(cr, accent[0], accent[1], accent[2],
				accent[3] * (0.30 + 0.70 * v));
		cairo_fill(cr);

		/* peak-hold cap: max level over the recent past, floating
		 * just above the bar and decaying as the bar falls away */
		for (int s = 1; s <= 4; s++) {
			double pv = spec_level(ph - s * 0.09, i);
			if (pv > peak)
				peak = pv;
		}
		cairo_rectangle(cr, x, base - (2.0 + env * peak) - 3.0,
				bar_w, 1.5);
		cairo_set_source_rgba(cr, accent[0], accent[1], accent[2],
				accent[3] * 0.85);
		cairo_fill(cr);
	}
	cairo_destroy(cr);
	return cairo_buf_finish(cs);
}

/* Soft drop shadow for a w×h card: layered rounded rects approximate a
 * gaussian falloff, biased slightly downward so the card reads as
 * floating above the content behind it. The card's own rect is punched
 * out so the translucent card body isn't darkened from behind. Buffer
 * is (w+2M)×(h+2M) with the card at (M,M); place the node at (-M,-M)
 * relative to the card. */
struct wlr_buffer *
card_shadow_buffer(int w, int h, double radius)
{
	const int M = CARD_SHADOW_MARGIN;
	const double yoff = 6.0;
	cairo_surface_t *cs;
	cairo_t *cr = cairo_buf_begin(w + 2 * M, h + 2 * M, &cs);

	if (!cr)
		return NULL;
	if (radius <= 0.0)
		radius = CARD_RADIUS;
	for (int i = M; i >= 1; i--) {
		double a = 0.046 * (1.0 - (double)i / (M + 1));

		rounded(cr, M - i, M - i + yoff, w + 2 * i, h + 2 * i,
				radius + i);
		cairo_set_source_rgba(cr, 0, 0, 0, a);
		cairo_fill(cr);
	}
	cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
	rounded(cr, M + 0.5, M + 0.5, w - 1.0, h - 1.0, radius);
	cairo_fill(cr);
	cairo_destroy(cr);
	return cairo_buf_finish(cs);
}

struct wlr_buffer *
card_panel_buffer(int w, int h)
{
	cairo_surface_t *cs;
	cairo_t *cr = cairo_buf_begin(w, h, &cs);

	if (!cr)
		return NULL;
	rounded(cr, 0.5, 0.5, w - 1.0, h - 1.0, CARD_RADIUS);
	cairo_set_source_rgba(cr, CARD_BG_R, CARD_BG_G, CARD_BG_B,
			card_bg_a(w, h));
	card_at_valid = 0;
	cairo_fill_preserve(cr);
	cairo_set_source_rgba(cr, 1, 1, 1, CARD_BORDER_A);
	cairo_set_line_width(cr, 1.0);
	cairo_stroke(cr);
	cairo_destroy(cr);
	return cairo_buf_finish(cs);
}

struct wlr_buffer *
card_hover_buffer(int w, int h)
{
	cairo_surface_t *cs;
	cairo_t *cr = cairo_buf_begin(w, h, &cs);

	if (!cr)
		return NULL;
	rounded(cr, 0, 0, w, h, 6);
	cairo_set_source_rgba(cr, 1, 1, 1, 0.10);
	cairo_fill(cr);
	cairo_destroy(cr);
	return cairo_buf_finish(cs);
}

struct wlr_buffer *
card_mark_buffer(int radio, int size, int state)
{
	cairo_surface_t *cs;
	cairo_t *cr = cairo_buf_begin(size, size, &cs);
	double s = size;

	if (!cr)
		return NULL;
	if (radio) {
		cairo_arc(cr, s / 2, s / 2, s / 2 - 1.0, 0, 2 * CARD_PI);
		cairo_set_source_rgba(cr, 1, 1, 1, 0.35);
		cairo_set_line_width(cr, 1.0);
		cairo_stroke(cr);
		if (state) {
			cairo_arc(cr, s / 2, s / 2, s * 0.22, 0, 2 * CARD_PI);
			cairo_set_source_rgba(cr, card_col_blue[0],
					card_col_blue[1], card_col_blue[2], 1.0);
			cairo_fill(cr);
		}
	} else if (state) {
		rounded(cr, 0.5, 0.5, s - 1.0, s - 1.0, 3.0);
		cairo_set_source_rgba(cr, card_col_blue[0], card_col_blue[1],
				card_col_blue[2], 0.9);
		cairo_fill(cr);
		cairo_set_source_rgba(cr, CARD_BG_R, CARD_BG_G, CARD_BG_B, 1.0);
		cairo_set_line_width(cr, 1.8);
		cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
		cairo_move_to(cr, s * 0.25, s * 0.55);
		cairo_line_to(cr, s * 0.44, s * 0.72);
		cairo_line_to(cr, s * 0.75, s * 0.30);
		cairo_stroke(cr);
	} else {
		rounded(cr, 0.5, 0.5, s - 1.0, s - 1.0, 3.0);
		cairo_set_source_rgba(cr, 1, 1, 1, 0.35);
		cairo_set_line_width(cr, 1.0);
		cairo_stroke(cr);
	}
	cairo_destroy(cr);
	return cairo_buf_finish(cs);
}

struct wlr_buffer *
card_chevron_buffer(int size)
{
	cairo_surface_t *cs;
	cairo_t *cr = cairo_buf_begin(size, size, &cs);
	double s = size;

	if (!cr)
		return NULL;
	cairo_set_source_rgba(cr, 1, 1, 1, 0.50);
	cairo_set_line_width(cr, 1.5);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
	cairo_move_to(cr, s * 0.38, s * 0.28);
	cairo_line_to(cr, s * 0.64, s * 0.50);
	cairo_line_to(cr, s * 0.38, s * 0.72);
	cairo_stroke(cr);
	cairo_destroy(cr);
	return cairo_buf_finish(cs);
}

/* ── shadow cache ────────────────────────────────────────────────── */

/* The shadow depends only on card size, but popups re-render their card
 * on every data refresh; the 24-layer falloff over (w+48)×(h+48) px is
 * the most expensive part of a re-render, so cache it per size. Cached
 * buffers hold their init reference until evicted — the returned buffer
 * is borrowed and must NOT be dropped by the caller. */
#define SHADOW_CACHE 8

static struct {
	int w, h;
	uint64_t used;
	struct wlr_buffer *buf;
} shadow_cache[SHADOW_CACHE];
static uint64_t shadow_cache_tick;

static struct wlr_buffer *
shadow_cached(int w, int h)
{
	int lru = 0;

	for (int i = 0; i < SHADOW_CACHE; i++) {
		if (shadow_cache[i].buf && shadow_cache[i].w == w &&
				shadow_cache[i].h == h) {
			shadow_cache[i].used = ++shadow_cache_tick;
			return shadow_cache[i].buf;
		}
		if (shadow_cache[i].used < shadow_cache[lru].used)
			lru = i;
	}
	if (shadow_cache[lru].buf)
		wlr_buffer_drop(shadow_cache[lru].buf);
	shadow_cache[lru].buf = card_shadow_buffer(w, h, 0);
	shadow_cache[lru].w = w;
	shadow_cache[lru].h = h;
	shadow_cache[lru].used = ++shadow_cache_tick;
	return shadow_cache[lru].buf;
}

/* ── presenter + animation ───────────────────────────────────────── */

#define ANIM_MAX 16

static PopupView *anim_views[ANIM_MAX];
static int anim_count;
static struct wl_event_source *anim_timer;

/* Apply the animation frame for `v` at time `now`; returns 1 while
 * still animating. */
static int
view_anim_frame(PopupView *v, uint64_t now)
{
	float t = 1.0f, ts = 1.0f;
	float e, es;
	uint64_t el;

	if (!v->animating || !v->card)
		return 0;
	el = now - v->anim_start_ms;
	if (el < CARD_SHOW_MS)
		t = (float)el / CARD_SHOW_MS;
	if (el < CARD_SWEEP_DELAY)
		ts = 0.0f;
	else if (el - CARD_SWEEP_DELAY < CARD_SWEEP_MS)
		ts = (float)(el - CARD_SWEEP_DELAY) / CARD_SWEEP_MS;
	e = ease_out_cubic(t);
	es = ease_out_cubic(ts);

	if (v->content)
		wlr_scene_node_set_position(&v->content->node, 0,
				-(int)lroundf(CARD_SLIDE_PX * (1.0f - e)));
	wlr_scene_buffer_set_opacity(v->card, e);
	if (v->shadow)
		wlr_scene_buffer_set_opacity(v->shadow, e);

	for (int i = 0; i < v->nfills; i++) {
		struct wlr_scene_buffer *fb = v->fills[i];
		int fw = v->fill_w[i], fh = v->fill_h[i];
		int rw;

		if (!fb)
			continue;
		wlr_scene_buffer_set_opacity(fb, e);
		rw = (int)lroundf(fw * es);
		if (rw < 1)
			rw = 1;
		v->fill_disp_w[i] = rw;
		wlr_scene_buffer_set_source_box(fb, &(struct wlr_fbox){
			.x = 0, .y = 0, .width = rw, .height = fh });
		wlr_scene_buffer_set_dest_size(fb, rw, fh);
	}

	if (t >= 1.0f && ts >= 1.0f) {
		v->animating = 0;
		return 0;
	}
	return 1;
}

/* Ease each fill's displayed width toward its target (slider moves);
 * returns 1 while any fill is still converging. */
static int
view_slider_frame(PopupView *v)
{
	int still = 0;

	for (int i = 0; i < v->nfills; i++) {
		struct wlr_scene_buffer *fb = v->fills[i];
		int target = v->fill_w[i], disp = v->fill_disp_w[i];
		int step;

		if (!fb || target == disp)
			continue;
		step = (target - disp) / 4;
		if (step == 0)
			step = target > disp ? 1 : -1;
		disp += step;
		v->fill_disp_w[i] = disp;
		wlr_scene_buffer_set_source_box(fb, &(struct wlr_fbox){
			.x = 0, .y = 0, .width = disp, .height = v->fill_h[i] });
		wlr_scene_buffer_set_dest_size(fb, disp, v->fill_h[i]);
		if (disp != target)
			still = 1;
	}
	return still;
}

static int
card_anim_tick(void *data)
{
	uint64_t now = monotonic_msec();
	int still = 0;

	(void)data;
	for (int i = 0; i < anim_count; i++) {
		if (view_anim_frame(anim_views[i], now) ||
				view_slider_frame(anim_views[i])) {
			still = 1;
		} else {
			memmove(&anim_views[i], &anim_views[i + 1],
					(size_t)(anim_count - i - 1) *
					sizeof(anim_views[0]));
			anim_count--;
			i--;
		}
	}
	if (still && anim_timer)
		wl_event_source_timer_update(anim_timer, 16);
	return 0;
}

static void
anim_register(PopupView *v)
{
	for (int i = 0; i < anim_count; i++)
		if (anim_views[i] == v)
			return;
	if (anim_count >= ANIM_MAX)
		return;
	anim_views[anim_count++] = v;
	if (!event_loop)
		return;
	if (!anim_timer)
		anim_timer = wl_event_loop_add_timer(event_loop, card_anim_tick,
				NULL);
	if (anim_timer)
		wl_event_source_timer_update(anim_timer, 1);
}

static void
anim_unregister(PopupView *v)
{
	for (int i = 0; i < anim_count; i++) {
		if (anim_views[i] == v) {
			memmove(&anim_views[i], &anim_views[i + 1],
					(size_t)(anim_count - i - 1) *
					sizeof(anim_views[0]));
			anim_count--;
			return;
		}
	}
}

void
popup_view_apply(PopupView *v, struct wlr_scene_tree *tree, CardResult *res)
{
	struct wlr_scene_node *node, *tmp;

	if (!v || !tree || !res || !res->buf)
		return;

	if (!v->content || v->content->node.parent != tree) {
		v->content = wlr_scene_tree_create(tree);
		if (!v->content)
			return;
	}
	wl_list_for_each_safe(node, tmp, &v->content->children, link)
		wlr_scene_node_destroy(node);
	v->card = NULL;
	v->shadow = NULL;
	memset(v->fills, 0, sizeof(v->fills));
	v->nfills = 0;

	/* drop shadow below the card so the popup floats (cached buffer —
	 * not dropped here) */
	{
		struct wlr_buffer *sh = shadow_cached(res->w, res->h);

		if (sh) {
			v->shadow = wlr_scene_buffer_create(v->content, NULL);
			if (v->shadow) {
				wlr_scene_buffer_set_buffer(v->shadow, sh);
				wlr_scene_node_set_position(&v->shadow->node,
						-CARD_SHADOW_MARGIN,
						-CARD_SHADOW_MARGIN);
			}
		}
	}

	v->card = wlr_scene_buffer_create(v->content, NULL);
	if (v->card) {
		wlr_scene_buffer_set_buffer(v->card, res->buf);
		wlr_scene_node_set_position(&v->card->node, 0, 0);
	}

	for (int i = 0; i < res->nfills && i < CARD_MAX_FILLS; i++) {
		CardFill *f = &res->fills[i];
		int buf_w = f->full_w > f->w ? f->full_w : f->w;
		struct wlr_buffer *fb = make_fill_buffer(buf_w, f->h, f->color);
		struct wlr_scene_buffer *sb;

		if (!fb)
			continue;
		sb = wlr_scene_buffer_create(v->content, NULL);
		if (sb) {
			wlr_scene_buffer_set_buffer(sb, fb);
			wlr_scene_node_set_position(&sb->node, f->x, f->y);
			wlr_scene_buffer_set_source_box(sb, &(struct wlr_fbox){
				.x = 0, .y = 0, .width = f->w, .height = f->h });
			wlr_scene_buffer_set_dest_size(sb, f->w, f->h);
			v->fills[v->nfills] = sb;
			v->fill_w[v->nfills] = f->w;
			v->fill_h[v->nfills] = f->h;
			v->fill_full_w[v->nfills] = buf_w;
			v->fill_disp_w[v->nfills] = f->w;
			v->nfills++;
		}
		wlr_buffer_drop(fb);
	}

	v->w = res->w;
	v->h = res->h;
	wlr_buffer_drop(res->buf);
	res->buf = NULL;

	/* re-applied mid-animation: restore the current frame state so a
	 * data refresh doesn't pop to full opacity */
	if (v->animating)
		view_anim_frame(v, monotonic_msec());
}

static int
fill_target_w(PopupView *v, int i, double frac)
{
	int target;

	if (frac < 0.0)
		frac = 0.0;
	if (frac > 1.0)
		frac = 1.0;
	target = (int)lround(v->fill_full_w[i] * frac);
	return target < 1 ? 1 : target;
}

void
popup_view_set_fill_frac(PopupView *v, int i, double frac)
{
	if (!v || i < 0 || i >= v->nfills || !v->fills[i])
		return;
	v->fill_w[i] = fill_target_w(v, i, frac);
	anim_register(v);
}

void
popup_view_drag_fill_frac(PopupView *v, int i, double frac)
{
	int target;

	if (!v || i < 0 || i >= v->nfills || !v->fills[i])
		return;
	target = fill_target_w(v, i, frac);
	v->fill_w[i] = target;
	v->fill_disp_w[i] = target;
	wlr_scene_buffer_set_source_box(v->fills[i], &(struct wlr_fbox){
		.x = 0, .y = 0, .width = target, .height = v->fill_h[i] });
	wlr_scene_buffer_set_dest_size(v->fills[i], target, v->fill_h[i]);
}

void
popup_view_show(PopupView *v)
{
	if (!v)
		return;
	v->anim_start_ms = monotonic_msec();
	v->animating = 1;
	view_anim_frame(v, v->anim_start_ms);
	anim_register(v);
}

void
popup_view_hide(PopupView *v)
{
	if (!v)
		return;
	v->animating = 0;
	anim_unregister(v);
	if (v->content)
		wlr_scene_node_set_position(&v->content->node, 0, 0);
	if (v->card)
		wlr_scene_buffer_set_opacity(v->card, 1.0f);
	if (v->shadow)
		wlr_scene_buffer_set_opacity(v->shadow, 1.0f);
}
