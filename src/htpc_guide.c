/*
 * htpc_guide.c — HTPC guide-button workspace menu.
 *
 * Pressing the gamepad guide button (BTN_MODE) in htpc mode opens a
 * compositor-drawn menu listing the fixed app workspaces (1 Steam,
 * 2 RetroArch, 3 GeForce NOW, 4 nixlymedia).  D-pad up/down moves the
 * selection, A switches to that workspace, B or guide closes.
 *
 * While the menu is open every gamepad is grabbed exclusively
 * (EVIOCGRAB via htpc_pad_grab), so navigating the menu never leaks
 * button presses into the app on screen; the grab is dropped the
 * moment the menu closes.  Apps are expected to ignore the guide
 * button itself (Steam: "Guide Button Focuses Steam" off, RetroArch:
 * menu_toggle bind stripped from the joypad autoconfig).
 *
 * Rendering follows osd.c: a cairo card buffer + fcft glyph scene
 * buffers in a tree on the overlay layer, centered on selmon.  All
 * sizes scale with the output (guide_metrics) so the card reads the
 * same from the couch on any TV resolution.
 */
#include <cairo/cairo.h>
#include <drm_fourcc.h>
#include <limits.h>
#include <linux/input-event-codes.h>

#include "nixlytile.h"
#include "client.h"

#define GUIDE_PI       3.14159265358979323846

static const char *entries[] = {
	"Steam",
	"RetroArch",
	"GeForce NOW",
	"nixlymedia",
};
#define GUIDE_N ((int)(sizeof(entries) / sizeof(entries[0])))

static struct wlr_scene_tree *menu_tree;
static Monitor *menu_mon;
static int menu_sel;
static int card_w, card_h;

/* Everything scales with the output so the card reads the same from
 * the couch on any TV: font ≈ 3.2 % of the monitor height, paddings
 * and rows in ems of that font, card at least 22 % of the width. */
static struct fcft_font *menu_font;
static int menu_font_px;
static int pad_x, pad_y, row_h, radius;

static struct fcft_font *
guide_font(void)
{
	return menu_font ? menu_font : statusfont.font;
}

static void
guide_metrics(void)
{
	int px;

	if (!menu_mon || !statusfont.font)
		return;
	px = menu_mon->m.height * 32 / 1000;
	if (px < statusfont.height)
		px = statusfont.height;
	if (!menu_font || menu_font_px != px) {
		if (menu_font)
			fcft_destroy(menu_font);
		menu_font = card_font_load((double)px / statusfont.height);
		menu_font_px = menu_font ? px : 0;
	}
	pad_x = px;
	pad_y = px * 3 / 4;
	row_h = px * 2;
	radius = px / 2;
}

int
htpc_guide_is_open(void)
{
	return menu_tree != NULL;
}

static void
guide_rounded_rect(cairo_t *cr, double x, double y, double w, double h,
		double r)
{
	cairo_new_sub_path(cr);
	cairo_arc(cr, x + w - r, y + r, r, -GUIDE_PI / 2, 0);
	cairo_arc(cr, x + w - r, y + h - r, r, 0, GUIDE_PI / 2);
	cairo_arc(cr, x + r, y + h - r, r, GUIDE_PI / 2, GUIDE_PI);
	cairo_arc(cr, x + r, y + r, r, GUIDE_PI, 3 * GUIDE_PI / 2);
	cairo_close_path(cr);
}

/* Card background with the selected row highlighted. */
static struct wlr_buffer *
guide_card_buffer(int w, int h, int sel)
{
	cairo_surface_t *cs;
	cairo_t *cr;
	struct PixmanBuffer *buf;
	void *data;
	int stride;

	cs = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
	if (cairo_surface_status(cs) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(cs);
		return NULL;
	}
	cr = cairo_create(cs);

	guide_rounded_rect(cr, 0.5, 0.5, w - 1.0, h - 1.0, radius);
	cairo_set_source_rgba(cr, 0.07, 0.08, 0.10, 0.96);
	cairo_fill_preserve(cr);
	cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.09);
	cairo_set_line_width(cr, 1.0);
	cairo_stroke(cr);

	guide_rounded_rect(cr,
			pad_x / 2.0,
			pad_y + (double)sel * row_h,
			w - pad_x, row_h, radius * 2 / 3);
	cairo_set_source_rgba(cr, focuscolor[0], focuscolor[1],
			focuscolor[2], 0.28);
	cairo_fill(cr);

	cairo_destroy(cr);
	cairo_surface_flush(cs);

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

static int
guide_text_width(const char *s)
{
	const struct fcft_glyph *glyph;
	struct fcft_font *f = guide_font();
	uint32_t prev_cp = 0;
	int pen_x = 0;

	for (size_t i = 0; s[i]; i++) {
		long kx = 0, ky = 0;
		uint32_t cp = (unsigned char)s[i];

		if (prev_cp)
			fcft_kerning(f, prev_cp, cp, &kx, &ky);
		pen_x += (int)kx;
		glyph = fcft_rasterize_char_utf32(f, cp,
				statusbar_font_subpixel);
		if (glyph)
			pen_x += glyph->advance.x;
		prev_cp = cp;
	}
	return pen_x;
}

static void
guide_draw_text(struct wlr_scene_tree *tree, const char *s, int x, int y)
{
	const struct fcft_glyph *glyph;
	struct fcft_font *f = guide_font();
	struct wlr_scene_buffer *sb;
	struct wlr_buffer *buffer;
	uint32_t prev_cp = 0;
	int pen_x = 0;

	for (size_t i = 0; s[i]; i++) {
		long kx = 0, ky = 0;
		uint32_t cp = (unsigned char)s[i];

		if (prev_cp)
			fcft_kerning(f, prev_cp, cp, &kx, &ky);
		pen_x += (int)kx;
		glyph = fcft_rasterize_char_utf32(f, cp,
				statusbar_font_subpixel);
		if (glyph && glyph->pix) {
			buffer = statusbar_buffer_from_glyph(glyph);
			if (buffer) {
				sb = wlr_scene_buffer_create(tree, NULL);
				if (sb) {
					wlr_scene_buffer_set_buffer(sb, buffer);
					wlr_scene_node_set_position(&sb->node,
							x + pen_x + glyph->x,
							y - glyph->y);
				}
				wlr_buffer_drop(buffer);
			}
		}
		if (glyph)
			pen_x += glyph->advance.x;
		prev_cp = cp;
	}
}

static void
guide_menu_build(void)
{
	struct wlr_scene_buffer *sb;
	struct wlr_buffer *buffer;
	struct wlr_scene_node *node, *tmp;
	struct fcft_font *f = guide_font();
	int i, w, text_h;

	if (!menu_tree || !menu_mon || !f)
		return;

	wl_list_for_each_safe(node, tmp, &menu_tree->children, link)
		wlr_scene_node_destroy(node);

	card_w = menu_mon->m.width * 22 / 100;
	for (i = 0; i < GUIDE_N; i++) {
		w = guide_text_width(entries[i]) + pad_x * 3;
		if (w > card_w)
			card_w = w;
	}
	card_h = pad_y * 2 + row_h * GUIDE_N;

	buffer = guide_card_buffer(card_w, card_h, menu_sel);
	if (buffer) {
		sb = wlr_scene_buffer_create(menu_tree, NULL);
		if (sb)
			wlr_scene_buffer_set_buffer(sb, buffer);
		wlr_buffer_drop(buffer);
	}

	/* Baseline roughly centered in each row. */
	text_h = f->ascent + f->descent;
	for (i = 0; i < GUIDE_N; i++)
		guide_draw_text(menu_tree, entries[i],
				pad_x + pad_x / 2,
				pad_y + i * row_h
					+ (row_h - text_h) / 2
					+ f->ascent);
}

static void
guide_menu_place(void)
{
	if (!menu_tree || !menu_mon)
		return;
	wlr_scene_node_set_position(&menu_tree->node,
			menu_mon->m.x + (menu_mon->m.width - card_w) / 2,
			menu_mon->m.y + (menu_mon->m.height - card_h) / 2);
	wlr_scene_node_raise_to_top(&menu_tree->node);
	if (menu_mon->wlr_output)
		wlr_output_schedule_frame(menu_mon->wlr_output);
}

void
htpc_guide_close(void)
{
	Monitor *m = menu_mon;

	if (!menu_tree)
		return;
	wlr_scene_node_destroy(&menu_tree->node);
	menu_tree = NULL;
	menu_mon = NULL;
	htpc_pad_grab(0);
	if (m && m->wlr_output)
		wlr_output_schedule_frame(m->wlr_output);
}

static void
guide_open(void)
{
	if (menu_tree || !htpc_mode_active || !selmon)
		return;
	menu_mon = selmon;
	menu_sel = (selmon->active_ws && selmon->active_ws->idx < GUIDE_N)
			? selmon->active_ws->idx : 0;
	menu_tree = wlr_scene_tree_create(layers[LyrOverlay]);
	if (!menu_tree) {
		menu_mon = NULL;
		return;
	}
	guide_metrics();
	guide_menu_build();
	guide_menu_place();
	htpc_pad_grab(1);
}

void
htpc_guide_toggle(void)
{
	if (menu_tree)
		htpc_guide_close();
	else
		guide_open();
}

/* Menu navigation from htpc_pad.c.  dir: -1 up, +1 down.  Returns 1
 * when the menu is open (event consumed). */
int
htpc_guide_nav(int dir)
{
	if (!menu_tree)
		return 0;
	menu_sel += dir;
	if (menu_sel < 0)
		menu_sel = GUIDE_N - 1;
	if (menu_sel >= GUIDE_N)
		menu_sel = 0;
	guide_menu_build();
	guide_menu_place();
	return 1;
}

int
htpc_guide_select(void)
{
	Arg a;
	int sel = menu_sel;

	if (!menu_tree)
		return 0;
	htpc_guide_close();
	a.i = sel;
	focus_workspace_n(&a);
	return 1;
}
