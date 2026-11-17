/* Frosted wallpaper behind the overview. */
#include "overview/state.h"

#define BLUR_W      160
#define BLUR_RADIUS 5
#define BLUR_PASSES 3

static struct nx_buffer *frosted;

typedef struct {
	uint32_t *px;
	int n, step;
} Strip;

static int
clamp_index(int i, int n)
{
	return i < 0 ? 0 : i >= n ? n - 1 : i;
}

static void
accumulate(int sum[static 3], uint32_t px, int sign)
{
	for (int k = 0; k < 3; k++)
		sum[k] += sign * (int)(px >> (16 - 8 * k) & 0xff);
}

/* Box blur along one line. */
static void
blur_line(Strip s, uint32_t *line)
{
	int span = 2 * BLUR_RADIUS + 1;
	int sum[3] = { 0 };

	for (int i = 0; i < s.n; i++)
		line[i] = s.px[i * s.step];
	for (int i = -BLUR_RADIUS; i <= BLUR_RADIUS; i++)
		accumulate(sum, line[clamp_index(i, s.n)], 1);
	for (int i = 0; i < s.n; i++) {
		s.px[i * s.step] = 0xff000000u | (uint32_t)(sum[0] / span) << 16
				| (uint32_t)(sum[1] / span) << 8 | (uint32_t)(sum[2] / span);
		accumulate(sum, line[clamp_index(i - BLUR_RADIUS, s.n)], -1);
		accumulate(sum, line[clamp_index(i + BLUR_RADIUS + 1, s.n)], 1);
	}
}

/* Box passes approximate gaussian. */
static void
blur(uint32_t *px, int w, int h)
{
	uint32_t *line = ecalloc((size_t)MAX(w, h), sizeof(*line));

	for (int pass = 0; pass < BLUR_PASSES; pass++) {
		for (int y = 0; y < h; y++)
			blur_line((Strip){ px + (size_t)y * (size_t)w, w, 1 }, line);
		for (int x = 0; x < w; x++)
			blur_line((Strip){ px + x, h, w }, line);
	}
	free(line);
}

static void
pack(GdkPixbuf *pb, uint32_t *px)
{
	int w = gdk_pixbuf_get_width(pb), h = gdk_pixbuf_get_height(pb);
	int nchan = gdk_pixbuf_get_n_channels(pb);
	int stride = gdk_pixbuf_get_rowstride(pb);
	const guchar *src = gdk_pixbuf_read_pixels(pb);

	for (int y = 0; y < h; y++) {
		const guchar *p = src + (size_t)y * (size_t)stride;

		for (int x = 0; x < w; x++, p += nchan)
			*px++ = 0xff000000u | (uint32_t)p[0] << 16
					| (uint32_t)p[1] << 8 | p[2];
	}
}

struct nx_buffer *
overview_blur(GdkPixbuf *wallpaper)
{
	int w = BLUR_W;
	int h = MAX(gdk_pixbuf_get_height(wallpaper) * BLUR_W
			/ MAX(gdk_pixbuf_get_width(wallpaper), 1), 1);
	GdkPixbuf *small = gdk_pixbuf_scale_simple(wallpaper, w, h,
			GDK_INTERP_TILES);
	struct PixmanBuffer *buf;

	if (!small)
		return NULL;
	if ((buf = bar_canvas_new(w, h))) {
		pack(small, buf->data);
		blur(buf->data, w, h);
		buf->drm_format = DRM_FORMAT_XRGB8888;
	}
	g_object_unref(small);
	return buf ? &buf->base : NULL;
}

void
overview_set_backdrop(struct nx_buffer *blur)
{
	nx_buffer_drop(frosted);
	frosted = blur;
}

/* Cropped like swaybg fill mode. */
struct nx_scene_buffer *
backdrop_attach(struct nx_scene_tree *parent, Monitor *m)
{
	struct nx_scene_buffer *b;
	double iw, ih, scale;

	if (!frosted)
		return NULL;
	iw = frosted->width;
	ih = frosted->height;
	scale = MAX(m->m.width / iw, m->m.height / ih);
	b = nx_scene_buffer_create(parent, frosted);
	nx_scene_buffer_set_source_box(b, &(struct nx_fbox){
			(iw - m->m.width / scale) / 2, (ih - m->m.height / scale) / 2,
			m->m.width / scale, m->m.height / scale });
	nx_scene_buffer_set_dest_size(b, m->m.width, m->m.height);
	nx_scene_node_set_position(&b->node, m->m.x, m->m.y);
	return b;
}
