#include "nixlytile.h"
#include "client.h"
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <wlr/render/dmabuf.h>

void
drawrect(struct wlr_scene_tree *parent, int x, int y,
		int width, int height, const float color[static 4])
{
	struct wlr_scene_rect *r;
	float col[4];

	if (!parent || width <= 0 || height <= 0)
		return;

	col[0] = color[0];
	col[1] = color[1];
	col[2] = color[2];
	col[3] = color[3];

	r = wlr_scene_rect_create(parent, width, height, col);
	if (r)
		wlr_scene_node_set_position(&r->node, x, y);
}

void
drawhoverrect(struct wlr_scene_tree *parent, int x, int y,
		int width, int height, const float color[static 4], float fade)
{
	struct wlr_scene_rect *r;
	float col[4];

	if (!parent || width <= 0 || height <= 0)
		return;

	if (fade < 0.0f)
		fade = 0.0f;
	if (fade > 1.0f)
		fade = 1.0f;

	col[0] = color[0];
	col[1] = color[1];
	col[2] = color[2];
	col[3] = color[3] * fade;

	r = wlr_scene_rect_create(parent, width, height, col);
	if (r)
		wlr_scene_node_set_position(&r->node, x, y);
}

void
draw_border(struct wlr_scene_tree *parent, int x, int y,
		int w, int h, int thickness, const float color[static 4])
{
	drawrect(parent, x, y, w, thickness, color);                /* top */
	drawrect(parent, x, y + h - thickness, w, thickness, color); /* bottom */
	drawrect(parent, x, y, thickness, h, color);                /* left */
	drawrect(parent, x + w - thickness, y, thickness, h, color); /* right */
}

void
drawroundedrect(struct wlr_scene_tree *parent, int x, int y,
		int width, int height, const float color[static 4])
{
	int radius, yy, inset_top, inset_bottom, inset, start, h, w;
	struct wlr_scene_rect *r;

	if (!parent || width <= 0 || height <= 0)
		return;

	radius = MIN(4, MIN(width, height) / 4);

	yy = 0;
	while (yy < height) {
		inset_top = radius ? MAX(0, radius - yy) : 0;
		inset_bottom = radius ? MAX(0, radius - ((height - 1) - yy)) : 0;
		inset = MAX(inset_top, inset_bottom);
		start = yy;

		while (yy < height) {
			inset_top = radius ? MAX(0, radius - yy) : 0;
			inset_bottom = radius ? MAX(0, radius - ((height - 1) - yy)) : 0;
			if (MAX(inset_top, inset_bottom) != inset)
				break;
			yy++;
		}

		h = yy - start;
		w = width - 2 * inset;
		if (h > 0 && w > 0) {
			r = wlr_scene_rect_create(parent, w, h, color);
			if (r)
				wlr_scene_node_set_position(&r->node, x + inset, y + start);
		}
	}
}

void
pixman_buffer_destroy(struct wlr_buffer *wlr_buffer)
{
	struct PixmanBuffer *buf = wl_container_of(wlr_buffer, buf, base);

	if (!buf)
		return;
	if (buf->image)
		pixman_image_unref(buf->image);
	if (buf->owns_data && buf->data)
		free(buf->data);
	free(buf);
}

bool
pixman_buffer_begin_data_ptr_access(struct wlr_buffer *wlr_buffer, uint32_t flags,
		void **data, uint32_t *format, size_t *stride)
{
	struct PixmanBuffer *buf = wl_container_of(wlr_buffer, buf, base);

	(void)flags;
	if (!buf || !data || !format || !stride)
		return false;

	*data = buf->data;
	*format = buf->drm_format;
	*stride = buf->stride;
	return true;
}

void
pixman_buffer_end_data_ptr_access(struct wlr_buffer *wlr_buffer)
{
	(void)wlr_buffer;
}

bool
pixman_buffer_get_dmabuf(struct wlr_buffer *wlr_buffer,
		struct wlr_dmabuf_attributes *attribs)
{
	(void)wlr_buffer;
	(void)attribs;
	return false;
}

bool
pixman_buffer_get_shm(struct wlr_buffer *wlr_buffer,
		struct wlr_shm_attributes *attribs)
{
	(void)wlr_buffer;
	(void)attribs;
	return false;
}

struct wlr_buffer *
statusbar_buffer_from_glyph(const struct fcft_glyph *glyph)
{
	pixman_image_t *dst, *solid = NULL;
	struct PixmanBuffer *buf;
	pixman_color_t col;
	uint32_t *data;
	int width, height, stride, force_color;

	if (!glyph || !glyph->pix)
		return NULL;

	width = glyph->width;
	height = glyph->height;
	if (width <= 0 || height <= 0)
		return NULL;

	stride = width * 4;
	data = ecalloc(height, stride);
	if (!data)
		return NULL;

	dst = pixman_image_create_bits_no_clear(PIXMAN_a8r8g8b8, width, height,
			data, stride);
	if (!dst) {
		free(data);
		return NULL;
	}

	force_color = statusbar_font_force_color || pixman_image_get_format(glyph->pix) == PIXMAN_a8;
	if (force_color) {
		const float *fg = statusbar_fg_override ? statusbar_fg_override : statusbar_fg;
		col.alpha = (uint16_t)lroundf(fg[3] * 65535.0f);
		col.red = (uint16_t)lroundf(fg[0] * 65535.0f);
		col.green = (uint16_t)lroundf(fg[1] * 65535.0f);
		col.blue = (uint16_t)lroundf(fg[2] * 65535.0f);
		solid = pixman_image_create_solid_fill(&col);
		pixman_image_composite32(PIXMAN_OP_SRC, solid, glyph->pix, dst,
				0, 0, 0, 0, 0, 0, width, height);
	} else {
		pixman_image_composite32(PIXMAN_OP_SRC, glyph->pix, NULL, dst,
				0, 0, 0, 0, 0, 0, width, height);
	}

	if (solid)
		pixman_image_unref(solid);

	buf = ecalloc(1, sizeof(*buf));
	if (!buf) {
		pixman_image_unref(dst);
		free(data);
		return NULL;
	}

	buf->image = dst;
	buf->data = data;
	buf->drm_format = DRM_FORMAT_ARGB8888;
	buf->stride = stride;
	buf->owns_data = 1;
	wlr_buffer_init(&buf->base, &pixman_buffer_impl, width, height);

	return &buf->base;
}

int
pathisdir(const char *path)
{
	struct stat st;

	return path && *path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int
pathisfile(const char *path)
{
	struct stat st;

	return path && *path && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

int
resolve_asset_path(const char *path, char *out, size_t len)
{
	static char exe_dir[PATH_MAX];
	static int exe_dir_cached = 0;
	char cand[PATH_MAX];
	const char *envdir;

	if (!path || !*path || !out || len == 0)
		return -1;

	if (path[0] == '/') {
		if (pathisfile(path)) {
			snprintf(out, len, "%s", path);
			return 0;
		}
		return -1;
	}

	if (pathisfile(path)) {
		snprintf(out, len, "%s", path);
		return 0;
	}

	envdir = getenv("NIXLYTILE_ICON_DIR");
	if (envdir && *envdir) {
		if (snprintf(cand, sizeof(cand), "%s/%s", envdir, path) < (int)sizeof(cand)
				&& pathisfile(cand)) {
			snprintf(out, len, "%s", cand);
			return 0;
		}
	}

	if (!exe_dir_cached) {
		char *slash;
		ssize_t n = readlink("/proc/self/exe", exe_dir, sizeof(exe_dir) - 1);
		if (n > 0 && (size_t)n < sizeof(exe_dir)) {
			exe_dir[n] = '\0';
			slash = strrchr(exe_dir, '/');
			if (slash)
				*slash = '\0';
			else
				exe_dir[0] = '\0';
		} else {
			exe_dir[0] = '\0';
		}
		exe_dir_cached = 1;
	}

	if (exe_dir[0]) {
		if (snprintf(cand, sizeof(cand), "%s/%s", exe_dir, path) < (int)sizeof(cand)
				&& pathisfile(cand)) {
			snprintf(out, len, "%s", cand);
			return 0;
		}
		if (snprintf(cand, sizeof(cand), "%s/../share/nixlytile/%s", exe_dir, path) < (int)sizeof(cand)
				&& pathisfile(cand)) {
			snprintf(out, len, "%s", cand);
			return 0;
		}
	}

	return -1;
}

int
has_svg_extension(const char *path)
{
	size_t len;

	if (!path || !*path)
		return 0;

	len = strlen(path);
	if (len >= 4 && strcasecmp(path + len - 4, ".svg") == 0)
		return 1;
	if (len >= 5 && strcasecmp(path + len - 5, ".svgz") == 0)
		return 1;
	return 0;
}

int
strip_symbolic_suffix(const char *name, char *out, size_t outlen)
{
	size_t len, suffix = strlen("-symbolic");

	if (!name || !out || outlen == 0)
		return 0;

	len = strlen(name);
	if (len > suffix && strcmp(name + len - suffix, "-symbolic") == 0) {
		size_t copylen = len - suffix;
		if (copylen >= outlen)
			copylen = outlen - 1;
		memcpy(out, name, copylen);
		out[copylen] = '\0';
		return 1;
	}
	return 0;
}

void
add_icon_root_paths(const char *base, const char *themes[], size_t theme_count,
		char pathbufs[][PATH_MAX], size_t *pathcount, size_t max_paths)
{
	if (!base || !*base || !themes || !pathbufs || !pathcount)
		return;

	for (size_t i = 0; i < theme_count && *pathcount < max_paths; i++) {
		char themed[PATH_MAX];
		snprintf(themed, sizeof(themed), "%s/%s", base, themes[i]);
		snprintf(pathbufs[*pathcount], PATH_MAX, "%s", themed);
		(*pathcount)++;
	}

	if (*pathcount < max_paths) {
		snprintf(pathbufs[*pathcount], PATH_MAX, "%s", base);
		(*pathcount)++;
	}
}

GdkPixbuf *
pixbuf_from_cairo_surface(cairo_surface_t *surface, int width, int height)
{
	GdkPixbuf *pixbuf;
	guchar *dst;
	const uint8_t *src;
	int dst_stride, src_stride;

	if (!surface || width <= 0 || height <= 0)
		return NULL;
	if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS)
		return NULL;

	cairo_surface_flush(surface);
	src = cairo_image_surface_get_data(surface);
	src_stride = cairo_image_surface_get_stride(surface);
	if (!src)
		return NULL;

	pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, 1, 8, width, height);
	if (!pixbuf)
		return NULL;
	dst = gdk_pixbuf_get_pixels(pixbuf);
	dst_stride = gdk_pixbuf_get_rowstride(pixbuf);

	for (int y = 0; y < height; y++) {
		const uint32_t *srow = (const uint32_t *)(src + (size_t)y * (size_t)src_stride);
		guchar *drow = dst + (size_t)y * (size_t)dst_stride;
		for (int x = 0; x < width; x++) {
			uint32_t s = srow[x];
			uint8_t a = (uint8_t)(s >> 24);
			uint8_t r = (uint8_t)(s >> 16);
			uint8_t g = (uint8_t)(s >> 8);
			uint8_t b = (uint8_t)s;

			if (a > 0) {
				/* Un-premultiply */
				r = (uint8_t)((((uint32_t)r) * 255u + a / 2u) / a);
				g = (uint8_t)((((uint32_t)g) * 255u + a / 2u) / a);
				b = (uint8_t)((((uint32_t)b) * 255u + a / 2u) / a);
			} else {
				r = g = b = 0;
			}

			drow[(size_t)x * 4] = r;
			drow[(size_t)x * 4 + 1] = g;
			drow[(size_t)x * 4 + 2] = b;
			drow[(size_t)x * 4 + 3] = a;
		}
	}

	return pixbuf;
}

void
tray_consider_icon(const char *path, int size_hint, int desired_h,
		char *best_path, int *best_diff, int *found)
{
	int diff;

	if (!path || !*path || !best_path || !best_diff || !found)
		return;
	if (!pathisfile(path) || access(path, R_OK) != 0)
		return;

	if (size_hint > 0 && desired_h > 0)
		diff = abs(size_hint - desired_h);
	else if (desired_h > 0 && size_hint <= 0)
		diff = desired_h;
	else
		diff = 0;

	if (!*found || diff < *best_diff) {
		*best_diff = diff;
		snprintf(best_path, PATH_MAX, "%s", path);
		*found = 1;
	}
}

struct wlr_buffer *
statusbar_buffer_from_pixbuf(GdkPixbuf *pixbuf, int target_h, int *out_w, int *out_h)
{
	GdkPixbuf *scaled = NULL;
	guchar *pixels;
	int w, h, nchan, stride;
	uint8_t *argb = NULL;
	size_t bufsize;
	struct wlr_buffer *buf = NULL;

	if (!pixbuf)
		return NULL;

	w = gdk_pixbuf_get_width(pixbuf);
	h = gdk_pixbuf_get_height(pixbuf);
	if (w <= 0 || h <= 0)
		goto out;

	if (target_h > 0 && h > target_h) {
		int target_w = (int)lround(((double)w * (double)target_h) / (double)h);
		if (target_w <= 0)
			target_w = 1;
		scaled = gdk_pixbuf_scale_simple(pixbuf, target_w, target_h, GDK_INTERP_BILINEAR);
		if (scaled) {
			g_object_unref(pixbuf);
			pixbuf = scaled;
			w = target_w;
			h = target_h;
		}
	}

	nchan = gdk_pixbuf_get_n_channels(pixbuf);
	if (nchan < 3)
		goto out;
	stride = gdk_pixbuf_get_rowstride(pixbuf);
	pixels = gdk_pixbuf_get_pixels(pixbuf);
	if (!pixels)
		goto out;

	bufsize = (size_t)w * (size_t)h * 4;
	argb = calloc(1, bufsize);
	if (!argb)
		goto out;

	for (int y = 0; y < h; y++) {
		const guchar *row = pixels + (size_t)y * (size_t)stride;
		for (int x = 0; x < w; x++) {
			const guchar *p = row + (size_t)x * (size_t)nchan;
			uint8_t r = p[0];
			uint8_t g = p[1];
			uint8_t b = p[2];
			uint8_t a = (nchan >= 4 && gdk_pixbuf_get_has_alpha(pixbuf)) ? p[3] : 255;
			size_t idx = ((size_t)y * (size_t)w + (size_t)x) * 4;
			argb[idx] = a;
			argb[idx + 1] = r;
			argb[idx + 2] = g;
			argb[idx + 3] = b;
		}
	}

	buf = statusbar_buffer_from_argb32((const uint32_t *)argb, w, h);
	if (buf) {
		if (out_w)
			*out_w = w;
		if (out_h)
			*out_h = h;
	}

out:
	free(argb);
	g_object_unref(pixbuf);
	return buf;
}

void
recolor_wifi100_pixbuf(GdkPixbuf *pixbuf)
{
	int w, h, nchan, stride;
	guchar *pixels;
	double fx, fy;
	int has_alpha;
	struct Rect {
		double x, y, w, h;
		uint8_t r, g, b;
	};
	static const struct Rect rects[] = {
		{ 6, 38, 10, 20, 0x82, 0xCC, 0x00 }, /* bar 1 main */
		{ 14, 38,  2, 20, 0x5E, 0x9E, 0x00 }, /* bar 1 stripe */
		{ 20, 30, 10, 28, 0x82, 0xCC, 0x00 }, /* bar 2 main */
		{ 28, 30,  2, 28, 0x5E, 0x9E, 0x00 }, /* bar 2 stripe */
		{ 34, 22, 10, 36, 0x82, 0xCC, 0x00 }, /* bar 3 main */
		{ 42, 22,  2, 36, 0x5E, 0x9E, 0x00 }, /* bar 3 stripe */
		{ 48, 14, 10, 44, 0x82, 0xCC, 0x00 }, /* bar 4 main */
		{ 56, 14,  2, 44, 0x5E, 0x9E, 0x00 }, /* bar 4 stripe */
	};

	if (!pixbuf)
		return;
	nchan = gdk_pixbuf_get_n_channels(pixbuf);
	if (nchan < 3)
		return;
	w = gdk_pixbuf_get_width(pixbuf);
	h = gdk_pixbuf_get_height(pixbuf);
	if (w <= 0 || h <= 0)
		return;
	stride = gdk_pixbuf_get_rowstride(pixbuf);
	pixels = gdk_pixbuf_get_pixels(pixbuf);
	if (!pixels)
		return;
	has_alpha = gdk_pixbuf_get_has_alpha(pixbuf);
	fx = (double)w / 64.0;
	fy = (double)h / 64.0;

	for (size_t i = 0; i < LENGTH(rects); i++) {
		int rx = (int)lround(rects[i].x * fx);
		int ry = (int)lround(rects[i].y * fy);
		int rw = (int)lround(rects[i].w * fx);
		int rh = (int)lround(rects[i].h * fy);
		if (rx < 0) rx = 0;
		if (ry < 0) ry = 0;
		if (rw < 1) rw = 1;
		if (rh < 1) rh = 1;
		if (rx + rw > w) rw = w - rx;
		if (ry + rh > h) rh = h - ry;
		for (int y = ry; y < ry + rh; y++) {
			guchar *row = pixels + (size_t)y * (size_t)stride;
			for (int x = rx; x < rx + rw; x++) {
				guchar *p = row + (size_t)x * (size_t)nchan;
				p[0] = rects[i].r;
				p[1] = rects[i].g;
				p[2] = rects[i].b;
				if (has_alpha && nchan >= 4)
					p[3] = 255;
			}
		}
	}
}

struct wlr_buffer *
statusbar_buffer_from_wifi100(int target_h, int *out_w, int *out_h)
{
	struct {
		int x, y, w, h;
		uint8_t r, g, b;
	} bars[] = {
		{6, 38, 10, 20, 0x82, 0xCC, 0x00}, /* bar 1 main */
		{14, 38, 2, 20, 0x5E, 0x9E, 0x00}, /* bar 1 stripe */
		{20, 30, 10, 28, 0x82, 0xCC, 0x00}, /* bar 2 main */
		{28, 30, 2, 28, 0x5E, 0x9E, 0x00}, /* bar 2 stripe */
		{34, 22, 10, 36, 0x82, 0xCC, 0x00}, /* bar 3 main */
		{42, 22, 2, 36, 0x5E, 0x9E, 0x00}, /* bar 3 stripe */
		{48, 14, 10, 44, 0x82, 0xCC, 0x00}, /* bar 4 main */
		{56, 14, 2, 44, 0x5E, 0x9E, 0x00}, /* bar 4 stripe */
	};
	const int base_w = 64;
	const int base_h = 64;
	int target_w, target_h_final;
	uint32_t *buf = NULL;
	struct wlr_buffer *wb = NULL;

	target_h_final = (target_h > 0) ? target_h : base_h;
	if (target_h_final <= 0)
		return NULL;
	target_w = (int)lround(((double)base_w * (double)target_h_final) / (double)base_h);
	if (target_w <= 0)
		target_w = 1;

	buf = calloc(1, (size_t)base_w * (size_t)base_h * sizeof(uint32_t));
	if (!buf)
		return NULL;

	for (size_t i = 0; i < LENGTH(bars); i++) {
		int rx = bars[i].x;
		int ry = bars[i].y;
		int rw = bars[i].w;
		int rh = bars[i].h;
		if (rx < 0) rx = 0;
		if (ry < 0) ry = 0;
		if (rw < 1) rw = 1;
		if (rh < 1) rh = 1;
		if (rx + rw > base_w) rw = base_w - rx;
		if (ry + rh > base_h) rh = base_h - ry;
		for (int y = ry; y < ry + rh; y++) {
			for (int x = rx; x < rx + rw; x++) {
				size_t idx = (size_t)y * (size_t)base_w + (size_t)x;
				buf[idx] = ((uint32_t)0xFF << 24)
						| ((uint32_t)bars[i].r << 16)
						| ((uint32_t)bars[i].g << 8)
						| (uint32_t)bars[i].b;
			}
		}
	}

	wb = statusbar_scaled_buffer_from_argb32_raw((const uint32_t *)buf, base_w, base_h, target_h_final);
	free(buf);
	if (wb) {
		if (out_w)
			*out_w = target_w;
		if (out_h)
			*out_h = target_h_final;
	}
	return wb;
}

int
tray_load_svg_pixbuf(const char *path, int desired_h, GdkPixbuf **out_pixbuf)
{
	RsvgHandle *handle = NULL;
	GError *gerr = NULL;
	double svg_w = 0.0, svg_h = 0.0;
	int target_w, target_h;
	cairo_surface_t *surface = NULL;
	cairo_t *cr = NULL;
	GdkPixbuf *pixbuf = NULL;

	if (!path || !*path || !out_pixbuf)
		return -1;

	/* Read the file manually so changes aren’t hidden by loader caches */
	{
		gchar *data = NULL;
		gsize len = 0;

		if (!g_file_get_contents(path, &data, &len, &gerr) || !data || len == 0) {
			if (gerr) {
				wlr_log(WLR_ERROR, "tray: failed to read SVG '%s': %s",
						path, gerr->message);
				g_error_free(gerr);
			}
			g_free(data);
			return -1;
		}

		handle = rsvg_handle_new_from_data((const guint8 *)data, len, &gerr);
		g_free(data);
	}
	if (!handle) {
		if (gerr) {
			wlr_log(WLR_ERROR, "tray: failed to parse SVG '%s': %s",
					path, gerr->message);
			g_error_free(gerr);
		}
		return -1;
	}

	if (!rsvg_handle_get_intrinsic_size_in_pixels(handle, &svg_w, &svg_h) ||
			svg_w <= 0.0 || svg_h <= 0.0) {
		RsvgDimensionData dim = {0};
		rsvg_handle_get_dimensions(handle, &dim);
		svg_w = dim.width;
		svg_h = dim.height;
	}

	target_h = (desired_h > 0) ? desired_h :
		((svg_h > 0.0) ? (int)lround(svg_h) : 24);
	if (target_h <= 0)
		target_h = 24;
	if (svg_w > 0.0 && svg_h > 0.0)
		target_w = MAX(1, (int)lround((svg_w * (double)target_h) / svg_h));
	else
		target_w = target_h;

	surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, target_w, target_h);
	if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS)
		goto out;

	cr = cairo_create(surface);
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.0);
	cairo_paint(cr);

	{
		RsvgRectangle viewport = {
			.x = 0,
			.y = 0,
			.width = (double)target_w,
			.height = (double)target_h,
		};
		if (!rsvg_handle_render_document(handle, cr, &viewport, &gerr)) {
			if (gerr) {
				wlr_log(WLR_ERROR, "tray: failed to render SVG '%s': %s",
						path, gerr->message);
				g_error_free(gerr);
				gerr = NULL;
			}
			goto out;
		}
	}

	cairo_destroy(cr);
	cr = NULL;
	pixbuf = pixbuf_from_cairo_surface(surface, target_w, target_h);

out:
	if (cr)
		cairo_destroy(cr);
	if (surface)
		cairo_surface_destroy(surface);
	if (handle)
		g_object_unref(handle);
	if (!pixbuf && gerr) {
		wlr_log(WLR_ERROR, "tray: failed to load SVG '%s': %s",
				path, gerr->message);
	}
	if (gerr)
		g_error_free(gerr);

	if (!pixbuf)
		return -1;

	*out_pixbuf = pixbuf;
	return 0;
}

void
fix_tray_argb32(uint32_t *pixels, size_t count, int use_rgba_order)
{
	size_t i;

	if (!pixels || count == 0)
		return;

	for (i = 0; i < count; i++) {
		/* IconPixmap spec: ARGB32, bytes in big-endian order A,R,G,B */
		const uint8_t *p8 = (const uint8_t *)&pixels[i];
		uint8_t a, r, g, b;

		if (use_rgba_order || statusbar_tray_force_rgba) {
			/* Fallback for apps that provide RGBA bytes */
			r = p8[0];
			g = p8[1];
			b = p8[2];
			a = p8[3];
		} else {
			a = p8[0];
			r = p8[1];
			g = p8[2];
			b = p8[3];
		}

		/* Premultiply for pixman/wlroots */
		if (a != 0 && a != 255) {
			r = (uint8_t)((r * a + 127) / 255);
			g = (uint8_t)((g * a + 127) / 255);
			b = (uint8_t)((b * a + 127) / 255);
		}

		pixels[i] = ((uint32_t)a << 24) | ((uint32_t)r << 16) |
			((uint32_t)g << 8) | (uint32_t)b;
	}
}

void
init_net_icon_paths(void)
{
	resolve_asset_path(net_icon_wifi_100, net_icon_wifi_100_resolved, sizeof(net_icon_wifi_100_resolved));
	resolve_asset_path(net_icon_wifi_75, net_icon_wifi_75_resolved, sizeof(net_icon_wifi_75_resolved));
	resolve_asset_path(net_icon_wifi_50, net_icon_wifi_50_resolved, sizeof(net_icon_wifi_50_resolved));
	resolve_asset_path(net_icon_wifi_25, net_icon_wifi_25_resolved, sizeof(net_icon_wifi_25_resolved));
	resolve_asset_path(net_icon_eth, net_icon_eth_resolved, sizeof(net_icon_eth_resolved));
	resolve_asset_path(net_icon_no_conn, net_icon_no_conn_resolved, sizeof(net_icon_no_conn_resolved));
	if (net_icon_no_conn_resolved[0])
		snprintf(net_icon_path, sizeof(net_icon_path), "%s", net_icon_no_conn_resolved);
	wlr_log(WLR_INFO, "net icon paths: wifi100=%s wifi75=%s wifi50=%s wifi25=%s eth=%s noconn=%s",
			net_icon_wifi_100_resolved[0] ? net_icon_wifi_100_resolved : net_icon_wifi_100,
			net_icon_wifi_75_resolved[0] ? net_icon_wifi_75_resolved : net_icon_wifi_75,
			net_icon_wifi_50_resolved[0] ? net_icon_wifi_50_resolved : net_icon_wifi_50,
			net_icon_wifi_25_resolved[0] ? net_icon_wifi_25_resolved : net_icon_wifi_25,
			net_icon_eth_resolved[0] ? net_icon_eth_resolved : net_icon_eth,
			net_icon_no_conn_resolved[0] ? net_icon_no_conn_resolved : net_icon_no_conn);
}

struct wlr_buffer *
statusbar_scaled_buffer_from_argb32_ex(const uint32_t *data, int width, int height,
		int target_h, int fix_format)
{
	pixman_image_t *src = NULL, *dst = NULL;
	struct PixmanBuffer *buf;
	uint8_t *src_copy = NULL;
	uint8_t *dst_copy = NULL;
	size_t src_stride, dst_stride, src_size, dst_size;
	int target_w;
	pixman_transform_t transform;

	if (!data || width <= 0 || height <= 0 || target_h <= 0)
		return NULL;

	target_w = (int)lround(((double)width * (double)target_h) / (double)height);
	if (target_w <= 0)
		target_w = 1;

	src_stride = (size_t)width * 4;
	dst_stride = (size_t)target_w * 4;
	src_size = src_stride * (size_t)height;
	dst_size = dst_stride * (size_t)target_h;

	src_copy = calloc(1, src_size);
	dst_copy = calloc(1, dst_size);
	if (!src_copy || !dst_copy)
		goto fail;
	memcpy(src_copy, data, src_size);

	if (fix_format) {
		fix_tray_argb32((uint32_t *)src_copy, (size_t)width * (size_t)height, 0);

		/* If the first pass produced fully transparent data, retry assuming RGBA order */
		size_t alpha_sum = 0;
		for (size_t n = 0; n < (size_t)width * (size_t)height; n++)
			alpha_sum += src_copy[n * 4];
		if (alpha_sum == 0) {
			memcpy(src_copy, data, src_size);
			fix_tray_argb32((uint32_t *)src_copy, (size_t)width * (size_t)height, 1);
		}
	}

	src = pixman_image_create_bits_no_clear(PIXMAN_a8r8g8b8, width, height,
			(uint32_t *)src_copy, (int)src_stride);
	dst = pixman_image_create_bits_no_clear(PIXMAN_a8r8g8b8, target_w, target_h,
			(uint32_t *)dst_copy, (int)dst_stride);
	if (!src || !dst)
		goto fail;

	pixman_transform_init_identity(&transform);
	pixman_transform_scale(&transform, NULL,
			pixman_double_to_fixed((double)width / (double)target_w),
			pixman_double_to_fixed((double)height / (double)target_h));
	pixman_image_set_transform(src, &transform);
	pixman_image_set_filter(src, PIXMAN_FILTER_BILINEAR, NULL, 0);

	pixman_image_composite32(PIXMAN_OP_SRC, src, NULL, dst,
			0, 0, 0, 0, 0, 0, target_w, target_h);

	pixman_image_unref(src);
	free(src_copy);
	src = NULL;
	src_copy = NULL;

	buf = ecalloc(1, sizeof(*buf));
	if (!buf)
		goto fail;

	buf->image = dst;
	buf->data = dst_copy;
	buf->drm_format = DRM_FORMAT_ARGB8888;
	buf->stride = (int)dst_stride;
	buf->owns_data = 1;
	wlr_buffer_init(&buf->base, &pixman_buffer_impl, target_w, target_h);
	return &buf->base;

fail:
	if (src)
		pixman_image_unref(src);
	if (dst)
		pixman_image_unref(dst);
	free(src_copy);
	free(dst_copy);
	return NULL;
}

struct wlr_buffer *
statusbar_scaled_buffer_from_argb32(const uint32_t *data, int width, int height, int target_h)
{
	return statusbar_scaled_buffer_from_argb32_ex(data, width, height, target_h, 1);
}

struct wlr_buffer *
statusbar_scaled_buffer_from_argb32_raw(const uint32_t *data, int width, int height, int target_h)
{
	return statusbar_scaled_buffer_from_argb32_ex(data, width, height, target_h, 0);
}

struct wlr_buffer *
statusbar_buffer_from_argb32_raw(const uint32_t *data, int width, int height)
{
	pixman_image_t *dst;
	struct PixmanBuffer *buf;
	uint8_t *copy;
	size_t stride, size;

	if (!data || width <= 0 || height <= 0)
		return NULL;

	stride = (size_t)width * 4;
	size = stride * (size_t)height;
	copy = calloc(1, size);
	if (!copy)
		return NULL;
	memcpy(copy, data, size);

	dst = pixman_image_create_bits_no_clear(PIXMAN_a8r8g8b8, width, height,
			(uint32_t *)copy, (int)stride);
	if (!dst) {
		free(copy);
		return NULL;
	}

	buf = ecalloc(1, sizeof(*buf));
	if (!buf) {
		pixman_image_unref(dst);
		free(copy);
		return NULL;
	}

	buf->image = dst;
	buf->data = copy;
	buf->drm_format = DRM_FORMAT_ARGB8888;
	buf->stride = (int)stride;
	buf->owns_data = 1;
	wlr_buffer_init(&buf->base, &pixman_buffer_impl, width, height);
	return &buf->base;
}

struct wlr_buffer *
statusbar_buffer_from_argb32(const uint32_t *data, int width, int height)
{
	pixman_image_t *dst;
	struct PixmanBuffer *buf;
	uint8_t *copy;
	size_t stride, size;

	if (!data || width <= 0 || height <= 0)
		return NULL;

	stride = (size_t)width * 4;
	size = stride * (size_t)height;
	copy = calloc(1, size);
	if (!copy)
		return NULL;
	memcpy(copy, data, size);
	fix_tray_argb32((uint32_t *)copy, (size_t)width * (size_t)height, 0);

	/* If the first pass produced fully transparent data, retry assuming RGBA order */
	{
		size_t alpha_sum = 0;
		for (size_t n = 0; n < (size_t)width * (size_t)height; n++)
			alpha_sum += copy[n * 4];
		if (alpha_sum == 0) {
			memcpy(copy, data, size);
			fix_tray_argb32((uint32_t *)copy, (size_t)width * (size_t)height, 1);
		}
	}

	dst = pixman_image_create_bits_no_clear(PIXMAN_a8r8g8b8, width, height,
			(uint32_t *)copy, (int)stride);
	if (!dst) {
		free(copy);
		return NULL;
	}

	buf = ecalloc(1, sizeof(*buf));
	if (!buf) {
		pixman_image_unref(dst);
		free(copy);
		return NULL;
	}

	buf->image = dst;
	buf->data = copy;
	buf->drm_format = DRM_FORMAT_ARGB8888;
	buf->stride = (int)stride;
	buf->owns_data = 1;
	wlr_buffer_init(&buf->base, &pixman_buffer_impl, width, height);

	return &buf->base;
}

int
loadstatusfont(void)
{
	size_t count;
	const char **fonts;

	if (statusfont.font)
		return 1;

	/* Use runtime fonts if configured, otherwise use default */
	if (runtime_fonts_set && runtime_fonts[0]) {
		fonts = (const char **)runtime_fonts;
		for (count = 0; count < 8 && fonts[count]; count++)
			;
	} else {
		fonts = statusbar_fonts;
		/* Count actual non-NULL fonts, not array size */
		for (count = 0; count < LENGTH(statusbar_fonts) && fonts[count]; count++)
			;
	}

	if (count == 0)
		return 0;

	statusfont.font = fcft_from_name(count, fonts, statusbar_font_attributes);
	if (!statusfont.font)
		return 0;

	statusfont.ascent = statusfont.font->ascent;
	statusfont.descent = statusfont.font->descent;
	statusfont.height = statusfont.font->height;
	return 1;
}

void
freestatusfont(void)
{
	if (statusfont.font)
		fcft_destroy(statusfont.font);
	statusfont.font = NULL;
}

/* ── CPU cursor buffer (Nvidia HW cursor plane) ──────────────────── */

static void
cpu_cursor_buffer_destroy_cb(struct wlr_buffer *wlr_buffer)
{
	struct CpuCursorBuffer *buf = wl_container_of(wlr_buffer, buf, base);

	if (buf->map && buf->map_size > 0)
		munmap(buf->map, buf->map_size);
	if (buf->dmabuf_fd >= 0)
		close(buf->dmabuf_fd);
	if (buf->gem_handle && buf->drm_fd >= 0) {
		struct drm_mode_destroy_dumb destroy = { .handle = buf->gem_handle };
		ioctl(buf->drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
	}
	if (buf->owns_drm_fd && buf->drm_fd >= 0)
		close(buf->drm_fd);
	free(buf);
}

static bool
cpu_cursor_buffer_get_dmabuf(struct wlr_buffer *wlr_buffer,
		struct wlr_dmabuf_attributes *attribs)
{
	struct CpuCursorBuffer *buf = wl_container_of(wlr_buffer, buf, base);

	if (buf->dmabuf_fd < 0)
		return false;

	memset(attribs, 0, sizeof(*attribs));
	attribs->width = (int32_t)buf->width;
	attribs->height = (int32_t)buf->height;
	attribs->format = DRM_FORMAT_ARGB8888;
	attribs->modifier = DRM_FORMAT_MOD_LINEAR;
	attribs->n_planes = 1;
	attribs->offset[0] = 0;
	attribs->stride[0] = buf->stride;
	attribs->fd[0] = buf->dmabuf_fd;
	return true;
}

static bool
cpu_cursor_buffer_begin_data_ptr_access(struct wlr_buffer *wlr_buffer,
		uint32_t flags, void **data, uint32_t *format, size_t *stride)
{
	struct CpuCursorBuffer *buf = wl_container_of(wlr_buffer, buf, base);
	(void)flags;

	if (!buf->map)
		return false;

	*data = buf->map;
	*format = DRM_FORMAT_ARGB8888;
	*stride = buf->stride;
	return true;
}

static void
cpu_cursor_buffer_end_data_ptr_access(struct wlr_buffer *wlr_buffer)
{
	(void)wlr_buffer;
}

static const struct wlr_buffer_impl cpu_cursor_buffer_impl = {
	.destroy = cpu_cursor_buffer_destroy_cb,
	.get_dmabuf = cpu_cursor_buffer_get_dmabuf,
	.begin_data_ptr_access = cpu_cursor_buffer_begin_data_ptr_access,
	.end_data_ptr_access = cpu_cursor_buffer_end_data_ptr_access,
};

struct CpuCursorBuffer *
cpu_cursor_buffer_create(int drm_fd, uint32_t w, uint32_t h, int owns_fd)
{
	struct CpuCursorBuffer *buf;
	struct drm_mode_create_dumb create = {0};
	struct drm_mode_map_dumb map_req = {0};
	int prime_fd = -1;

	buf = calloc(1, sizeof(*buf));
	if (!buf)
		return NULL;

	buf->drm_fd = drm_fd;
	buf->owns_drm_fd = owns_fd;
	buf->dmabuf_fd = -1;

	/* Create dumb buffer */
	create.width = w;
	create.height = h;
	create.bpp = 32;
	if (ioctl(drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) < 0) {
		wlr_log(WLR_ERROR, "CPU cursor: DRM_IOCTL_MODE_CREATE_DUMB failed: %s",
			strerror(errno));
		free(buf);
		return NULL;
	}
	buf->gem_handle = create.handle;
	buf->width = w;
	buf->height = h;
	buf->stride = create.pitch;
	buf->map_size = (size_t)create.size;

	/* Map for CPU access */
	map_req.handle = buf->gem_handle;
	if (ioctl(drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &map_req) < 0) {
		wlr_log(WLR_ERROR, "CPU cursor: DRM_IOCTL_MODE_MAP_DUMB failed: %s",
			strerror(errno));
		goto fail;
	}

	buf->map = mmap(NULL, buf->map_size, PROT_READ | PROT_WRITE, MAP_SHARED,
			drm_fd, map_req.offset);
	if (buf->map == MAP_FAILED) {
		buf->map = NULL;
		wlr_log(WLR_ERROR, "CPU cursor: mmap failed: %s", strerror(errno));
		goto fail;
	}

	/* Export as DMA-BUF for wlroots */
	if (drmPrimeHandleToFD(drm_fd, buf->gem_handle,
			DRM_CLOEXEC | DRM_RDWR, &prime_fd) < 0) {
		wlr_log(WLR_ERROR, "CPU cursor: drmPrimeHandleToFD failed: %s",
			strerror(errno));
		goto fail;
	}
	buf->dmabuf_fd = prime_fd;

	/* Clear to transparent */
	memset(buf->map, 0, buf->map_size);

	wlr_buffer_init(&buf->base, &cpu_cursor_buffer_impl, (int)w, (int)h);

	wlr_log(WLR_INFO, "CPU cursor buffer created: %ux%u stride=%u size=%zu",
		w, h, buf->stride, buf->map_size);
	return buf;

fail:
	if (buf->map)
		munmap(buf->map, buf->map_size);
	if (prime_fd >= 0)
		close(prime_fd);
	if (buf->gem_handle) {
		struct drm_mode_destroy_dumb destroy = { .handle = buf->gem_handle };
		ioctl(drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
	}
	free(buf);
	return NULL;
}

void
cpu_cursor_buffer_destroy(struct CpuCursorBuffer *buf)
{
	if (!buf)
		return;
	wlr_buffer_drop(&buf->base);
}

void
nixly_cursor_set_xcursor(const char *name)
{
	struct wlr_xcursor *xcur;
	struct wlr_xcursor_image *img;
	uint32_t src_stride, copy_w, copy_h, y;
	const uint8_t *src;
	uint8_t *dst;

	if (!cpu_cursor_active) {
		wlr_cursor_set_xcursor(cursor, cursor_mgr, name);
		return;
	}

	xcur = wlr_xcursor_manager_get_xcursor(cursor_mgr, name, 1);
	if (!xcur || xcur->image_count == 0) {
		wlr_cursor_set_xcursor(cursor, cursor_mgr, name);
		return;
	}

	img = xcur->images[0];

	/* Clear buffer to transparent */
	memset(cpu_cursor_buf->map, 0, cpu_cursor_buf->map_size);

	/* Copy xcursor pixels into dumb buffer (row by row, handle stride) */
	copy_w = MIN(img->width, cpu_cursor_buf->width);
	copy_h = MIN(img->height, cpu_cursor_buf->height);
	src_stride = img->width * 4;

	src = img->buffer;
	dst = (uint8_t *)cpu_cursor_buf->map;

	for (y = 0; y < copy_h; y++) {
		memcpy(dst + y * cpu_cursor_buf->stride,
			src + y * src_stride,
			copy_w * 4);
	}

	wlr_cursor_set_buffer(cursor, &cpu_cursor_buf->base,
		(int32_t)img->hotspot_x, (int32_t)img->hotspot_y, 1.0f);
}

void
nixly_cursor_set_client_surface(struct wlr_surface *surface, int hx, int hy)
{
	void *src_data;
	uint32_t src_format;
	size_t src_stride;
	uint32_t copy_w, copy_h, y;
	uint8_t *dst;

	if (!cpu_cursor_active || !surface || !surface->buffer) {
		wlr_cursor_set_surface(cursor, surface, hx, hy);
		return;
	}

	/* Try to get pixel data from client buffer */
	if (!wlr_buffer_begin_data_ptr_access(&surface->buffer->base,
			WLR_BUFFER_DATA_PTR_ACCESS_READ,
			&src_data, &src_format, &src_stride)) {
		/* DMA-BUF client cursor — can't CPU-copy, fall through */
		wlr_cursor_set_surface(cursor, surface, hx, hy);
		return;
	}

	/* Clear buffer to transparent */
	memset(cpu_cursor_buf->map, 0, cpu_cursor_buf->map_size);

	/* Copy client cursor pixels into dumb buffer */
	copy_w = MIN((uint32_t)surface->current.width, cpu_cursor_buf->width);
	copy_h = MIN((uint32_t)surface->current.height, cpu_cursor_buf->height);

	dst = (uint8_t *)cpu_cursor_buf->map;

	for (y = 0; y < copy_h; y++) {
		memcpy(dst + y * cpu_cursor_buf->stride,
			(uint8_t *)src_data + y * src_stride,
			copy_w * 4);
	}

	wlr_buffer_end_data_ptr_access(&surface->buffer->base);

	wlr_cursor_set_buffer(cursor, &cpu_cursor_buf->base,
		(int32_t)hx, (int32_t)hy, 1.0f);
}

