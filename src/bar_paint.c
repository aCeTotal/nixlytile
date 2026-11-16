#include "nixlytile.h"
#include "popup_card.h"

typedef struct {
	struct fcft_font *font;
	int px;
} SizedFont;

static SizedFont text_font, icon_font;

void
bar_color_premul(const float c[static 4], float a, float out[static 4])
{
	out[3] = c[3] * a;
	out[0] = c[0] * out[3];
	out[1] = c[1] * out[3];
	out[2] = c[2] * out[3];
}

static struct fcft_font *
sized_font(SizedFont *sf, int px)
{
	struct fcft_font *f;

	if (sf->font && px == sf->px)
		return sf->font;
	if (!statusfont.font || statusfont.height <= 0)
		return sf->font;
	f = card_font_load((double)px / statusfont.height);
	if (!f)
		return sf->font;
	if (sf->font)
		fcft_destroy(sf->font);
	sf->font = f;
	sf->px = px;
	return f;
}

struct fcft_font *
bar_font_get(int bar_h)
{
	return sized_font(&text_font, (int)lround(bar_h * BAR_FONT_RATIO));
}

/* Zeroed ARGB canvas, or NULL. */
struct PixmanBuffer *
bar_canvas_new(int w, int h)
{
	struct PixmanBuffer *buf = ecalloc(1, sizeof(*buf));

	buf->stride = w * 4;
	buf->data = ecalloc(1, (size_t)buf->stride * (size_t)h);
	buf->image = pixman_image_create_bits(PIXMAN_a8r8g8b8, w, h,
			buf->data, buf->stride);
	buf->drm_format = DRM_FORMAT_ARGB8888;
	buf->owns_data = 1;
	nx_buffer_init(&buf->base, &pixman_buffer_impl, w, h);
	if (buf->image)
		return buf;
	nx_buffer_drop(&buf->base);
	return NULL;
}

static void
draw_icon(pixman_image_t *dst, const struct fcft_glyph *g,
		const float col[static 4])
{
	int y = (pixman_image_get_height(dst) - g->height) / 2;
	pixman_color_t pc = {
		.red = (uint16_t)lroundf(col[0] * col[3] * 65535.0f),
		.green = (uint16_t)lroundf(col[1] * col[3] * 65535.0f),
		.blue = (uint16_t)lroundf(col[2] * col[3] * 65535.0f),
		.alpha = (uint16_t)lroundf(col[3] * 65535.0f),
	};
	pixman_image_t *solid;

	if (pixman_image_get_format(g->pix) == PIXMAN_a8r8g8b8) {
		pixman_image_composite32(PIXMAN_OP_OVER, g->pix, NULL, dst,
				0, 0, 0, 0, 0, y, g->width, g->height);
		return;
	}
	solid = pixman_image_create_solid_fill(&pc);
	pixman_image_composite32(PIXMAN_OP_OVER, solid, g->pix, dst,
			0, 0, 0, 0, 0, y, g->width, g->height);
	pixman_image_unref(solid);
}

static const struct fcft_glyph *
icon_glyph(int bar_h, uint32_t cp)
{
	struct fcft_font *f;
	const struct fcft_glyph *g;

	if (!cp)
		return NULL;
	f = sized_font(&icon_font, (int)lround(bar_h * BAR_ICON_RATIO));
	if (!f)
		return NULL;
	g = fcft_rasterize_char_utf32(f, cp, FCFT_SUBPIXEL_NONE);
	return g && g->pix && g->width > 0 ? g : NULL;
}

int
bar_icon_width(int bar_h, uint32_t cp)
{
	const struct fcft_glyph *g = icon_glyph(bar_h, cp);

	return g ? g->width : 0;
}

#define ICON_SIZES 4

/* Popup icon sizes; the oldest slot is recycled. */
const struct fcft_glyph *
bar_glyph_px(uint32_t cp, int px)
{
	static SizedFont sizes[ICON_SIZES];
	static int next;
	SizedFont *sf = NULL;
	struct fcft_font *f;
	const struct fcft_glyph *g;

	for (int i = 0; i < ICON_SIZES && !sf; i++)
		if (sizes[i].font && sizes[i].px == px)
			sf = &sizes[i];
	if (!sf) {
		sf = &sizes[next];
		next = (next + 1) % ICON_SIZES;
	}
	if (!(f = sized_font(sf, px)))
		return NULL;
	g = fcft_rasterize_char_utf32(f, cp, FCFT_SUBPIXEL_NONE);
	return g && g->pix && g->width > 0 ? g : NULL;
}

/* Icon plus text, transparent strip. */
struct nx_buffer *
bar_label_buffer(const BarLabel *l, int h)
{
	struct fcft_font *f = bar_font_get(h);
	const struct fcft_glyph *g;
	struct PixmanBuffer *buf;
	int icon_w, text_w = 0, gap = 0, w;

	if (!f || h <= 0)
		return NULL;
	g = icon_glyph(h, l->icon);
	icon_w = g ? g->width : 0;
	if (l->text && *l->text)
		text_w = nixly_text_width(f, l->text);
	if (icon_w && text_w)
		gap = BAR_ICON_GAP;
	w = icon_w + gap + text_w;
	if (w <= 0 || !(buf = bar_canvas_new(w, h)))
		return NULL;
	if (g)
		draw_icon(buf->image, g, l->icon_fg);
	if (text_w)
		nixly_text_draw(buf->image, f, l->text, icon_w + gap,
				(h + f->ascent - f->descent) / 2, l->text_fg, 0);
	return &buf->base;
}

static uint32_t
pack_premul(const float c[static 4])
{
	return (uint32_t)lroundf(c[3] * 255.0f) << 24
		| (uint32_t)lroundf(c[0] * 255.0f) << 16
		| (uint32_t)lroundf(c[1] * 255.0f) << 8
		| (uint32_t)lroundf(c[2] * 255.0f);
}

static float
coverage(double r, double dist)
{
	double c = r - dist + 0.5;

	return (float)(c < 0.0 ? 0.0 : c > 1.0 ? 1.0 : c);
}

/* Fill, then a 1 px rim over it. */
static uint32_t
disc_pixel(double r, double dist, float cols[2][4])
{
	float body = coverage(r, dist);
	float rim = body - coverage(r - 1.0, dist);
	float px[4];

	for (int i = 0; i < 4; i++)
		px[i] = cols[1][i] * rim + cols[0][i] * body * (1.0f - cols[1][3] * rim);
	return pack_premul(px);
}

/* Anti-aliased pill cap disc. */
struct nx_buffer *
bar_disc_buffer(int d, const float fill[static 4], const float edge[static 4])
{
	struct PixmanBuffer *buf;
	float cols[2][4];
	double r = d / 2.0;
	uint32_t *px;

	if (d <= 0 || !(buf = bar_canvas_new(d, d)))
		return NULL;
	bar_color_premul(fill, 1.0f, cols[0]);
	bar_color_premul(edge, 1.0f, cols[1]);
	px = buf->data;
	for (int i = 0; i < d * d; i++)
		px[i] = disc_pixel(r, hypot(i % d + 0.5 - r, i / d + 0.5 - r), cols);
	return &buf->base;
}
