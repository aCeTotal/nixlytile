#include "nixlytile.h"
#include "client.h"
#include "wobble.h"

#include <math.h>
#include <stdlib.h>

typedef struct {
	struct wlr_render_pass *pass;
	struct wlr_render_texture_options tex;
	struct wlr_fbox src;
	double scale, pad, height;       /* logical px */
	int width_px, max_x;
} WobblePaint;

/* Window geometry crop, buffer px. */
static struct wlr_fbox
wobble_source(Client *c, struct wlr_surface *surf)
{
	struct wlr_fbox src;
	struct wlr_box geo;
	double x0, y0, x1, y1, sx, sy;

	wlr_surface_get_buffer_source_box(surf, &src);
	if (client_is_x11(c) || surf->current.width <= 0 || surf->current.height <= 0)
		return src;
	client_get_geometry(c, &geo);
	x0 = fmax(0.0, geo.x);
	y0 = fmax(0.0, geo.y);
	x1 = fmin(surf->current.width, geo.x + geo.width);
	y1 = fmin(surf->current.height, geo.y + geo.height);
	if (x1 <= x0 || y1 <= y0)
		return src;
	sx = src.width / surf->current.width;
	sy = src.height / surf->current.height;
	return (struct wlr_fbox){ src.x + x0 * sx, src.y + y0 * sy,
		(x1 - x0) * sx, (y1 - y0) * sy };
}

/* One run per pixel of shear. */
static void
wobble_paint_band(const Wobble *w, WobblePaint *p, int i)
{
	double top = (p->height * i / WOBBLE_EDGES + w->off[1][i] + p->pad) * p->scale;
	double bottom = (p->height * (i + 1) / WOBBLE_EDGES + w->off[1][i + 1] + p->pad) * p->scale;
	double from = (w->off[0][i] + p->pad) * p->scale;
	double to = (w->off[0][i + 1] + p->pad) * p->scale;
	double src_h = p->src.height / WOBBLE_EDGES;
	double src_y = p->src.y + src_h * i;
	int steps = abs((int)(lround(to) - lround(from))) + 1;
	int runs = MIN(steps, MAX(1, (int)(lround(bottom) - lround(top))));
	int j;

	for (j = 0; j < runs; j++) {
		int y0 = (int)lround(top + (bottom - top) * j / runs);
		int y1 = (int)lround(top + (bottom - top) * (j + 1) / runs);

		if (y1 <= y0)
			continue;
		p->tex.src_box = (struct wlr_fbox){ p->src.x, src_y + src_h * j / runs,
			p->src.width, src_h / runs };
		p->tex.dst_box = (struct wlr_box){
			MIN(p->max_x, (int)lround(from + (to - from) * (j + 0.5) / runs)),
			y0, p->width_px, y1 - y0 };
		wlr_render_pass_add_texture(p->pass, &p->tex);
	}
}

static int
wobble_draw(Client *c, Wobble *w, struct wlr_buffer *buffer)
{
	struct wlr_surface *surf = client_surface(c);
	struct wlr_linux_drm_syncobj_surface_v1_state *sync =
		wlr_linux_drm_syncobj_v1_get_surface_state(surf);
	double scale = c->mon->wlr_output->scale;
	WobblePaint p = {
		.tex = {
			.texture = surf->buffer->texture,
			.filter_mode = WLR_SCALE_FILTER_BILINEAR,
			.blend_mode = WLR_RENDER_BLEND_MODE_NONE,
			.wait_timeline = sync ? sync->acquire_timeline : NULL,
			.wait_point = sync ? sync->acquire_point : 0,
		},
		.src = wobble_source(c, surf),
		.scale = scale,
		.pad = ceil(w->cap),
		.height = c->geom.height - 2 * (int)c->bw,
		.width_px = (int)lround((c->geom.width - 2 * (int)c->bw) * scale),
	};
	int i;

	p.max_x = buffer->width - p.width_px;
	p.pass = wlr_renderer_begin_buffer_pass(drw, buffer, NULL);
	if (!p.pass)
		return 0;
	wlr_render_pass_add_rect(p.pass, &(struct wlr_render_rect_options){
		.box = { .width = buffer->width, .height = buffer->height },
		.blend_mode = WLR_RENDER_BLEND_MODE_NONE,
	});
	for (i = 0; i < WOBBLE_EDGES; i++)
		wobble_paint_band(w, &p, i);
	return wlr_render_pass_submit(p.pass);
}

/* GBM picks a renderable modifier. */
static const struct wlr_drm_format *
wobble_format(void)
{
	const struct wlr_drm_format_set *formats =
		wlr_renderer_get_texture_formats(drw, WLR_BUFFER_CAP_DMABUF);

	return formats ? wlr_drm_format_set_get(formats, DRM_FORMAT_ARGB8888) : NULL;
}

static struct wlr_buffer *
wobble_acquire(Wobble *w, int width, int height)
{
	const struct wlr_drm_format *format;

	if (w->swapchain && w->swapchain->width == width
			&& w->swapchain->height == height)
		return wlr_swapchain_acquire(w->swapchain);
	wlr_swapchain_destroy(w->swapchain);
	w->swapchain = NULL;
	format = wobble_format();
	if (format)
		w->swapchain = wlr_swapchain_create(alloc, width, height, format);
	return w->swapchain ? wlr_swapchain_acquire(w->swapchain) : NULL;
}

int
wobble_render(Client *c, Wobble *w)
{
	double scale = c->mon->wlr_output->scale;
	int bw = (int)c->bw;
	int pad = (int)ceil(w->cap);
	int width = c->geom.width - 2 * bw + 2 * pad;
	int height = c->geom.height - 2 * bw + 2 * pad;
	struct wlr_buffer *buffer = wobble_acquire(w,
			(int)ceil(width * scale), (int)ceil(height * scale));
	int drawn;

	if (!buffer)
		return 0;
	drawn = wobble_draw(c, w, buffer);
	if (drawn)
		wlr_scene_buffer_set_buffer(w->view, buffer);
	wlr_buffer_unlock(buffer);
	if (!drawn)
		return 0;
	wlr_scene_buffer_set_dest_size(w->view, width, height);
	wlr_scene_node_set_position(&w->view->node, bw - pad, bw - pad);
	return 1;
}
