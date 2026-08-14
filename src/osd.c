/*
 * osd.c — compositor-drawn toast notifications.
 *
 * Modern card (rounded corners, dark translucent background, hairline
 * border, accent stripe) that slides in from the right edge, holds, and
 * slides back out — same feel as Steam's notifications and the client
 * notification lane in notify.c.  Used for the compositor's own messages
 * (game mode, VRR, FPS limit, Hz/mode changes).
 *
 * One toast per monitor: a new message while one is visible swaps the
 * card content in place and restarts the hold, so rapid-fire updates
 * (FPS-limit scroll) update a single card instead of stacking.
 */
#include <cairo/cairo.h>
#include <math.h>

#include "nixlytile.h"
#include "client.h"

#define OSD_MARGIN     16    /* distance from screen edge */
#define OSD_HOLD_MS    3000
#define OSD_PAD_X      22
#define OSD_PAD_Y      16
#define OSD_RADIUS     14
#define OSD_ACCENT_W   4
#define OSD_PI         3.14159265358979323846

/* Same critically-damped feel as the client notification lane. */
static const SpringParams SPRING_OSD = { 1.0, 1.0, 800.0 };

typedef struct Toast {
	struct wl_list link;
	Monitor *m;
	struct wlr_scene_tree *tree;
	int w, h;
	int slot_y;
	int target_x, off_x;
	double x_f, x_vel;
	int hiding;
	struct wl_event_source *timer;
} Toast;

static struct wl_list toasts = { &toasts, &toasts };

static void
osd_schedule(Monitor *m)
{
	if (m && m->wlr_output)
		wlr_output_schedule_frame(m->wlr_output);
}

static void
toast_destroy(Toast *t)
{
	if (t->timer)
		wl_event_source_remove(t->timer);
	if (t->tree)
		wlr_scene_node_destroy(&t->tree->node);
	wl_list_remove(&t->link);
	free(t);
}

static int
osd_hide_timeout(void *data)
{
	Toast *t = data;

	t->hiding = 1;
	t->target_x = t->off_x;
	osd_schedule(t->m);
	return 0;
}

/* Rounded-rect path helper. */
static void
rounded_rect(cairo_t *cr, double x, double y, double w, double h, double r)
{
	cairo_new_sub_path(cr);
	cairo_arc(cr, x + w - r, y + r, r, -OSD_PI / 2, 0);
	cairo_arc(cr, x + w - r, y + h - r, r, 0, OSD_PI / 2);
	cairo_arc(cr, x + r, y + h - r, r, OSD_PI / 2, OSD_PI);
	cairo_arc(cr, x + r, y + r, r, OSD_PI, 3 * OSD_PI / 2);
	cairo_close_path(cr);
}

/* Card background: dark translucent rounded rect, hairline border, and
 * an accent stripe hugging the left rounded edge. */
static struct wlr_buffer *
make_card_buffer(int w, int h)
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

	/* Body */
	rounded_rect(cr, 0.5, 0.5, w - 1.0, h - 1.0, OSD_RADIUS);
	cairo_set_source_rgba(cr, 0.07, 0.08, 0.10, 0.94);
	cairo_fill_preserve(cr);
	/* Hairline border */
	cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.09);
	cairo_set_line_width(cr, 1.0);
	cairo_stroke(cr);

	/* Accent stripe (focuscolor) clipped to the rounded left edge. */
	rounded_rect(cr, 0.5, 0.5, w - 1.0, h - 1.0, OSD_RADIUS);
	cairo_clip(cr);
	cairo_rectangle(cr, 0, 0, OSD_ACCENT_W, h);
	cairo_set_source_rgba(cr, focuscolor[0], focuscolor[1],
			focuscolor[2], 1.0);
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

/* (Re)build the card contents in t->tree and set t->w/t->h.
 * Returns 0 when nothing could be rendered. */
static int
toast_build(Toast *t, const char *msg)
{
	tll(struct GlyphRun) glyphs = tll_init();
	const struct fcft_glyph *glyph;
	struct wlr_scene_buffer *scene_buf;
	struct wlr_buffer *buffer;
	struct wlr_scene_node *node, *tmp;
	uint32_t prev_cp = 0;
	int pen_x = 0;
	int min_y = INT_MAX, max_y = INT_MIN;
	int text_x;

	if (!statusfont.font || !msg || !*msg)
		return 0;

	for (size_t i = 0; msg[i]; i++) {
		long kern_x = 0, kern_y = 0;
		uint32_t cp = (unsigned char)msg[i];

		if (prev_cp)
			fcft_kerning(statusfont.font, prev_cp, cp, &kern_x, &kern_y);
		pen_x += (int)kern_x;

		glyph = fcft_rasterize_char_utf32(statusfont.font, cp,
				statusbar_font_subpixel);
		if (glyph && glyph->pix) {
			tll_push_back(glyphs, ((struct GlyphRun){
				.glyph = glyph,
				.pen_x = pen_x,
				.codepoint = cp,
			}));
			if (-glyph->y < min_y)
				min_y = -glyph->y;
			if (-glyph->y + (int)glyph->height > max_y)
				max_y = -glyph->y + (int)glyph->height;
		}
		if (glyph)
			pen_x += glyph->advance.x;
		prev_cp = cp;
	}

	if (tll_length(glyphs) == 0) {
		tll_free(glyphs);
		return 0;
	}

	t->w = pen_x + OSD_ACCENT_W + OSD_PAD_X * 2;
	t->h = (max_y - min_y) + OSD_PAD_Y * 2;
	text_x = OSD_ACCENT_W + OSD_PAD_X;

	wl_list_for_each_safe(node, tmp, &t->tree->children, link)
		wlr_scene_node_destroy(node);

	buffer = make_card_buffer(t->w, t->h);
	if (buffer) {
		scene_buf = wlr_scene_buffer_create(t->tree, NULL);
		if (scene_buf) {
			wlr_scene_buffer_set_buffer(scene_buf, buffer);
			wlr_scene_node_set_position(&scene_buf->node, 0, 0);
		}
		wlr_buffer_drop(buffer);
	}

	{
		int origin_y = OSD_PAD_Y - min_y;

		tll_foreach(glyphs, it) {
			glyph = it->item.glyph;
			buffer = statusbar_buffer_from_glyph(glyph);
			if (!buffer)
				continue;
			scene_buf = wlr_scene_buffer_create(t->tree, NULL);
			if (scene_buf) {
				wlr_scene_buffer_set_buffer(scene_buf, buffer);
				wlr_scene_node_set_position(&scene_buf->node,
						text_x + it->item.pen_x + glyph->x,
						origin_y - glyph->y);
			}
			wlr_buffer_drop(buffer);
		}
	}

	tll_free(glyphs);
	return 1;
}

/* Klipp kortet mot sin egen skjermkant: treet glir i globale
 * layout-koordinater, så delen utenfor høyre kant ville ellers blitt tegnet
 * på naboskjermen.  Alle barna i t->tree er scene-buffere (kort + glyfer),
 * ukalerte, så en 1:1 source-box-crop holder. */
static void
toast_clip_to_mon(Toast *t, int x)
{
	struct wlr_scene_node *node;
	int lim = t->m->m.x + t->m->m.width - x;

	wl_list_for_each(node, &t->tree->children, link) {
		struct wlr_scene_buffer *sb = wlr_scene_buffer_from_node(node);
		int bw = sb->buffer ? sb->buffer->width : 0;
		int bh = sb->buffer ? sb->buffer->height : 0;
		int vis = lim - node->x;

		if (bw <= 0)
			continue;
		if (vis >= bw) {
			wlr_scene_node_set_enabled(node, 1);
			wlr_scene_buffer_set_source_box(sb, NULL);
			wlr_scene_buffer_set_dest_size(sb, bw, bh);
		} else if (vis <= 0) {
			wlr_scene_node_set_enabled(node, 0);
		} else {
			struct wlr_fbox src = { 0, 0, vis, bh };
			wlr_scene_node_set_enabled(node, 1);
			wlr_scene_buffer_set_source_box(sb, &src);
			wlr_scene_buffer_set_dest_size(sb, vis, bh);
		}
	}
}

/* Put a card on screen, no questions asked.  osd_show() gates on game
 * mode; the launch cover uses this directly for its one "Game Mode On". */
void
osd_show_force(Monitor *m, const char *msg)
{
	Toast *t;

	if (!m || !m->wlr_output || !msg || !*msg)
		return;

	/* Swap content in place when a toast is already up on this monitor:
	 * keeps rapid-fire updates (FPS-limit scroll) in one card. */
	wl_list_for_each(t, &toasts, link) {
		if (t->m != m)
			continue;
		if (!toast_build(t, msg))
			return;
		/* Re-anchor (width may have changed) and slide back in if it
		 * was on its way out. */
		t->off_x = m->m.x + m->m.width + OSD_MARGIN;
		t->target_x = m->m.x + m->m.width - t->w - OSD_MARGIN;
		t->hiding = 0;
		if (t->timer)
			wl_event_source_timer_update(t->timer, OSD_HOLD_MS);
		/* Stay above the launch cover, which lives in the same layer. */
		wlr_scene_node_raise_to_top(&t->tree->node);
		toast_clip_to_mon(t, (int)t->x_f);
		osd_schedule(m);
		return;
	}

	t = calloc(1, sizeof(*t));
	if (!t)
		return;
	t->m = m;
	t->tree = wlr_scene_tree_create(layers[LyrOverlay]);
	if (!t->tree) {
		free(t);
		return;
	}
	if (!toast_build(t, msg)) {
		wlr_scene_node_destroy(&t->tree->node);
		free(t);
		return;
	}

	t->slot_y = m->w.y + OSD_MARGIN;
	t->target_x = m->m.x + m->m.width - t->w - OSD_MARGIN;
	t->off_x = m->m.x + m->m.width + OSD_MARGIN;
	t->x_f = (double)t->off_x;
	t->x_vel = 0.0;
	t->hiding = 0;
	wlr_scene_node_set_position(&t->tree->node, t->off_x, t->slot_y);
	toast_clip_to_mon(t, t->off_x);

	t->timer = wl_event_loop_add_timer(event_loop, osd_hide_timeout, t);
	if (t->timer)
		wl_event_source_timer_update(t->timer, OSD_HOLD_MS);

	wl_list_insert(&toasts, &t->link);
	osd_schedule(m);
}

void
osd_show(Monitor *m, const char *msg)
{
	/* A game launch or game mode retunes VRR, refresh and modes across
	 * every output — those toasts popped up on all the other monitors
	 * while the game was starting.  No compositor messages from the Play
	 * press until game mode ends. */
	if (game_mode_active || game_mode_ultra || launchfx_active())
		return;
	osd_show_force(m, msg);
}

void
osd_tick(Monitor *m, double dt, int *still)
{
	Toast *t, *tmp;

	*still = 0;
	wl_list_for_each_safe(t, tmp, &toasts, link) {
		if (t->m != m)
			continue;
		if (spring_tick(&t->x_f, &t->x_vel, (double)t->target_x,
				SPRING_OSD, dt)) {
			wlr_scene_node_set_position(&t->tree->node,
					(int)t->x_f, t->slot_y);
			toast_clip_to_mon(t, (int)t->x_f);
			*still = 1;
			continue;
		}
		wlr_scene_node_set_position(&t->tree->node,
				t->target_x, t->slot_y);
		toast_clip_to_mon(t, t->target_x);
		if (t->hiding)
			toast_destroy(t);
	}
}

/* Monitor is being destroyed — drop its toasts before m is freed. */
void
osd_purge_mon(Monitor *m)
{
	Toast *t, *tmp;

	wl_list_for_each_safe(t, tmp, &toasts, link)
		if (t->m == m)
			toast_destroy(t);
}
