/* screenshot.c - Built-in screenshot selection tool */
#include "nixlytile.h"
#include "client.h"

/*
 * Screenshot state machine:
 *   SCREENSHOT_NONE (0)      — normal
 *   SCREENSHOT_PENDING (1)   — waiting for rendermon() to capture pixels
 *   SCREENSHOT_SELECTING (2) — overlay visible, waiting for mouse down
 *   SCREENSHOT_DRAGGING (3)  — mouse down, selection rectangle being drawn
 */

/* Captured frame data */
static uint32_t *screenshot_pixels;
static int screenshot_width;
static int screenshot_height;
static int screenshot_stride;

/* Selection coordinates (layout space) */
static double sel_start_x, sel_start_y;
static double sel_cur_x, sel_cur_y;

/* Overlay scene nodes */
static struct wlr_scene_tree *screenshot_overlay_tree;
static struct wlr_scene_rect *dim_rects[4]; /* top, bottom, left, right */

/* Monitor that owns the screenshot */
static Monitor *screenshot_mon;

/* ── PNG write callback for Cairo ──────────────────────────────────── */

typedef struct {
	void *data;
	size_t len;
	size_t cap;
} PngBuffer;

static cairo_status_t
png_write_cb(void *closure, const unsigned char *data, unsigned int length)
{
	PngBuffer *buf = closure;
	if (buf->len + length > buf->cap) {
		size_t newcap = buf->cap ? buf->cap * 2 : 65536;
		while (newcap < buf->len + length)
			newcap *= 2;
		void *newdata = realloc(buf->data, newcap);
		if (!newdata)
			return CAIRO_STATUS_WRITE_ERROR;
		buf->data = newdata;
		buf->cap = newcap;
	}
	memcpy((char *)buf->data + buf->len, data, length);
	buf->len += length;
	return CAIRO_STATUS_SUCCESS;
}

/* ── Clipboard data source ─────────────────────────────────────────── */

struct screenshot_source {
	struct wlr_data_source base;
	void *png_data;
	size_t png_size;
};

/* Async write state for non-blocking clipboard send */
struct screenshot_write_state {
	void *data;
	size_t size;
	size_t written;
	int fd;
	struct wl_event_source *event_source;
};

static int
screenshot_write_handler(int fd, uint32_t mask, void *data)
{
	struct screenshot_write_state *ws = data;

	if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR))
		goto done;

	while (ws->written < ws->size) {
		ssize_t n = write(fd, (char *)ws->data + ws->written,
			ws->size - ws->written);
		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return 0; /* wait for next writable event */
			goto done;
		}
		if (n == 0)
			goto done;
		ws->written += n;
	}

done:
	wl_event_source_remove(ws->event_source);
	close(fd);
	free(ws);
	return 0;
}

static void
screenshot_source_send(struct wlr_data_source *source,
	const char *mime_type, int fd)
{
	struct screenshot_source *src =
		wl_container_of(source, src, base);

	if (strcmp(mime_type, "image/png") != 0) {
		close(fd);
		return;
	}

	/* Set non-blocking so we never stall the compositor */
	int flags = fcntl(fd, F_GETFL);
	fcntl(fd, F_SETFL, flags | O_NONBLOCK);

	/* Try to write as much as possible immediately */
	size_t written = 0;
	while (written < src->png_size) {
		ssize_t n = write(fd, (char *)src->png_data + written,
			src->png_size - written);
		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break;
			close(fd);
			return;
		}
		if (n == 0) {
			close(fd);
			return;
		}
		written += n;
	}

	if (written >= src->png_size) {
		close(fd);
		return;
	}

	/* Remaining data — hand off to event loop */
	struct screenshot_write_state *ws = calloc(1, sizeof(*ws));
	if (!ws) {
		close(fd);
		return;
	}
	ws->data = src->png_data;
	ws->size = src->png_size;
	ws->written = written;
	ws->fd = fd;
	ws->event_source = wl_event_loop_add_fd(event_loop, fd,
		WL_EVENT_WRITABLE, screenshot_write_handler, ws);
	if (!ws->event_source) {
		close(fd);
		free(ws);
	}
}

static void
screenshot_source_destroy(struct wlr_data_source *source)
{
	struct screenshot_source *src =
		wl_container_of(source, src, base);
	free(src->png_data);
	free(src);
}

static const struct wlr_data_source_impl screenshot_source_impl = {
	.send = screenshot_source_send,
	.destroy = screenshot_source_destroy,
};

/* ── Overlay management ────────────────────────────────────────────── */

static void
screenshot_create_overlay(void)
{
	if (!screenshot_mon)
		return;

	screenshot_overlay_tree = wlr_scene_tree_create(layers[LyrBlock]);
	if (!screenshot_overlay_tree)
		return;

	/* Single full-screen dim rect initially */
	float dim_color[4] = {0.0f, 0.0f, 0.0f, 0.5f};
	int w = screenshot_mon->m.width;
	int h = screenshot_mon->m.height;

	/* top, bottom, left, right — initially top covers everything */
	dim_rects[0] = wlr_scene_rect_create(screenshot_overlay_tree, w, h, dim_color);
	wlr_scene_node_set_position(&dim_rects[0]->node,
		screenshot_mon->m.x, screenshot_mon->m.y);

	dim_rects[1] = wlr_scene_rect_create(screenshot_overlay_tree, 0, 0, dim_color);
	dim_rects[2] = wlr_scene_rect_create(screenshot_overlay_tree, 0, 0, dim_color);
	dim_rects[3] = wlr_scene_rect_create(screenshot_overlay_tree, 0, 0, dim_color);
}

static void
screenshot_update_overlay(int x1, int y1, int x2, int y2)
{
	if (!screenshot_overlay_tree || !screenshot_mon)
		return;

	int mx = screenshot_mon->m.x;
	int my = screenshot_mon->m.y;
	int mw = screenshot_mon->m.width;
	int mh = screenshot_mon->m.height;

	/* Clamp selection to monitor bounds */
	if (x1 < mx) x1 = mx;
	if (y1 < my) y1 = my;
	if (x2 > mx + mw) x2 = mx + mw;
	if (y2 > my + mh) y2 = my + mh;

	int sel_x = x1, sel_y = y1;
	int sel_w = x2 - x1, sel_h = y2 - y1;
	if (sel_w < 1) sel_w = 1;
	if (sel_h < 1) sel_h = 1;

	/* Top rect: full width, from monitor top to selection top */
	wlr_scene_rect_set_size(dim_rects[0], mw, sel_y - my);
	wlr_scene_node_set_position(&dim_rects[0]->node, mx, my);

	/* Bottom rect: full width, from selection bottom to monitor bottom */
	int bot_y = sel_y + sel_h;
	int bot_h = (my + mh) - bot_y;
	if (bot_h < 0) bot_h = 0;
	wlr_scene_rect_set_size(dim_rects[1], mw, bot_h);
	wlr_scene_node_set_position(&dim_rects[1]->node, mx, bot_y);

	/* Left rect: from selection top to selection bottom, monitor left to selection left */
	int left_w = sel_x - mx;
	if (left_w < 0) left_w = 0;
	wlr_scene_rect_set_size(dim_rects[2], left_w, sel_h);
	wlr_scene_node_set_position(&dim_rects[2]->node, mx, sel_y);

	/* Right rect: from selection top to selection bottom, selection right to monitor right */
	int right_x = sel_x + sel_w;
	int right_w = (mx + mw) - right_x;
	if (right_w < 0) right_w = 0;
	wlr_scene_rect_set_size(dim_rects[3], right_w, sel_h);
	wlr_scene_node_set_position(&dim_rects[3]->node, right_x, sel_y);
}

static void
screenshot_destroy_overlay(void)
{
	if (screenshot_overlay_tree) {
		wlr_scene_node_destroy(&screenshot_overlay_tree->node);
		screenshot_overlay_tree = NULL;
	}
	dim_rects[0] = dim_rects[1] = dim_rects[2] = dim_rects[3] = NULL;
}

/* ── Cleanup ───────────────────────────────────────────────────────── */

static void
screenshot_cleanup(void)
{
	screenshot_destroy_overlay();
	free(screenshot_pixels);
	screenshot_pixels = NULL;
	screenshot_width = 0;
	screenshot_height = 0;
	screenshot_stride = 0;
	screenshot_mon = NULL;
	screenshot_mode = SCREENSHOT_NONE;

	/* Restore normal cursor */
	wlr_cursor_set_xcursor(cursor, cursor_mgr, "default");
}

/* ── Copy selection to clipboard ───────────────────────────────────── */

static void
screenshot_copy_to_clipboard(int x1, int y1, int x2, int y2)
{
	if (!screenshot_pixels || !screenshot_mon)
		return;

	/* Convert layout-space coords to pixel-space */
	float scale = screenshot_mon->wlr_output->scale;
	int px1 = (int)((x1 - screenshot_mon->m.x) * scale);
	int py1 = (int)((y1 - screenshot_mon->m.y) * scale);
	int px2 = (int)((x2 - screenshot_mon->m.x) * scale);
	int py2 = (int)((y2 - screenshot_mon->m.y) * scale);

	/* Clamp to buffer bounds */
	if (px1 < 0) px1 = 0;
	if (py1 < 0) py1 = 0;
	if (px2 > screenshot_width) px2 = screenshot_width;
	if (py2 > screenshot_height) py2 = screenshot_height;

	int sel_w = px2 - px1;
	int sel_h = py2 - py1;
	if (sel_w <= 0 || sel_h <= 0) {
		screenshot_cleanup();
		return;
	}

	/* Crop pixels */
	int src_stride_px = screenshot_stride / 4;
	uint32_t *cropped = malloc(sel_w * sel_h * 4);
	if (!cropped) {
		screenshot_cleanup();
		return;
	}
	for (int row = 0; row < sel_h; row++) {
		memcpy(cropped + row * sel_w,
			screenshot_pixels + (py1 + row) * src_stride_px + px1,
			sel_w * 4);
	}

	/* Encode PNG via Cairo */
	cairo_surface_t *surf = cairo_image_surface_create_for_data(
		(unsigned char *)cropped, CAIRO_FORMAT_ARGB32,
		sel_w, sel_h, sel_w * 4);

	PngBuffer png_buf = {0};
	cairo_status_t status = cairo_surface_write_to_png_stream(surf, png_write_cb, &png_buf);
	cairo_surface_destroy(surf);
	free(cropped);

	if (status != CAIRO_STATUS_SUCCESS || !png_buf.data) {
		free(png_buf.data);
		screenshot_cleanup();
		return;
	}

	/* Create data source and set clipboard */
	struct screenshot_source *src = calloc(1, sizeof(*src));
	if (!src) {
		free(png_buf.data);
		screenshot_cleanup();
		return;
	}

	static const char *mime_types[] = { "image/png" };
	wlr_data_source_init(&src->base, &screenshot_source_impl);
	wl_array_init(&src->base.mime_types);
	char **p = wl_array_add(&src->base.mime_types, sizeof(char *));
	*p = strdup(mime_types[0]);

	src->png_data = png_buf.data;
	src->png_size = png_buf.len;

	wlr_seat_set_selection(seat, &src->base,
		wl_display_next_serial(dpy));

	wlr_log(WLR_INFO, "Screenshot: %dx%d region copied to clipboard (%zu bytes PNG)",
		sel_w, sel_h, png_buf.len);

	screenshot_cleanup();
}

/* ── Public API ────────────────────────────────────────────────────── */

void
screenshot_begin(const Arg *arg)
{
	(void)arg;

	if (screenshot_mode != SCREENSHOT_NONE) {
		screenshot_cleanup();
		return;
	}

	if (!selmon) return;

	screenshot_mon = selmon;
	screenshot_mode = SCREENSHOT_PENDING;

	/* Force full damage so wlr_scene_output_build_state() produces a
	 * composited buffer we can read pixels from. Without damage the
	 * scene graph may skip rendering (no new content) and build_state
	 * returns false, which causes rendermon() to return before the
	 * screenshot capture code is reached. */
	wlr_damage_ring_add_whole(&screenshot_mon->scene_output->damage_ring);
	wlr_output_schedule_frame(screenshot_mon->wlr_output);
}

void
screenshot_capture_frame(Monitor *m, struct wlr_buffer *buffer)
{
	if (screenshot_mode != SCREENSHOT_PENDING || m != screenshot_mon)
		return;

	if (!buffer) {
		screenshot_cleanup();
		return;
	}

	/* Create texture from the buffer */
	struct wlr_texture *tex = wlr_texture_from_buffer(drw, buffer);
	if (!tex) {
		wlr_log(WLR_ERROR, "Screenshot: failed to create texture from buffer");
		screenshot_cleanup();
		return;
	}

	screenshot_width = tex->width;
	screenshot_height = tex->height;
	screenshot_stride = screenshot_width * 4;
	screenshot_pixels = malloc(screenshot_stride * screenshot_height);
	if (!screenshot_pixels) {
		wlr_texture_destroy(tex);
		screenshot_cleanup();
		return;
	}

	if (!wlr_texture_read_pixels(tex,
			&(struct wlr_texture_read_pixels_options){
		.data = screenshot_pixels,
		.format = DRM_FORMAT_ARGB8888,
		.stride = screenshot_stride,
		.src_box = { .width = screenshot_width, .height = screenshot_height },
	})) {
		wlr_log(WLR_ERROR, "Screenshot: failed to read pixels from texture");
		wlr_texture_destroy(tex);
		screenshot_cleanup();
		return;
	}

	wlr_texture_destroy(tex);

	/* Create overlay and switch to selecting mode */
	screenshot_create_overlay();
	screenshot_mode = SCREENSHOT_SELECTING;

	/* Set crosshair cursor */
	wlr_cursor_set_xcursor(cursor, cursor_mgr, "crosshair");

	wlr_log(WLR_INFO, "Screenshot: captured %dx%d frame, selecting region",
		screenshot_width, screenshot_height);
}

void
screenshot_handle_button(uint32_t button, uint32_t state, uint32_t time_msec)
{
	(void)time_msec;

	if (button == BTN_RIGHT && state == WL_POINTER_BUTTON_STATE_PRESSED) {
		screenshot_cleanup();
		return;
	}

	if (button != BTN_LEFT)
		return;

	if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
		if (screenshot_mode == SCREENSHOT_SELECTING) {
			sel_start_x = cursor->x;
			sel_start_y = cursor->y;
			sel_cur_x = cursor->x;
			sel_cur_y = cursor->y;
			screenshot_mode = SCREENSHOT_DRAGGING;
		}
	} else if (state == WL_POINTER_BUTTON_STATE_RELEASED) {
		if (screenshot_mode == SCREENSHOT_DRAGGING) {
			sel_cur_x = cursor->x;
			sel_cur_y = cursor->y;

			int x1 = (int)(sel_start_x < sel_cur_x ? sel_start_x : sel_cur_x);
			int y1 = (int)(sel_start_y < sel_cur_y ? sel_start_y : sel_cur_y);
			int x2 = (int)(sel_start_x > sel_cur_x ? sel_start_x : sel_cur_x);
			int y2 = (int)(sel_start_y > sel_cur_y ? sel_start_y : sel_cur_y);

			if (x2 - x1 < 2 || y2 - y1 < 2) {
				/* Too small, cancel */
				screenshot_cleanup();
				return;
			}

			screenshot_copy_to_clipboard(x1, y1, x2, y2);
		}
	}
}

void
screenshot_handle_motion(void)
{
	if (screenshot_mode != SCREENSHOT_DRAGGING)
		return;

	sel_cur_x = cursor->x;
	sel_cur_y = cursor->y;

	int x1 = (int)(sel_start_x < sel_cur_x ? sel_start_x : sel_cur_x);
	int y1 = (int)(sel_start_y < sel_cur_y ? sel_start_y : sel_cur_y);
	int x2 = (int)(sel_start_x > sel_cur_x ? sel_start_x : sel_cur_x);
	int y2 = (int)(sel_start_y > sel_cur_y ? sel_start_y : sel_cur_y);

	screenshot_update_overlay(x1, y1, x2, y2);
}

void
screenshot_handle_key(xkb_keysym_t sym)
{
	if (sym == XKB_KEY_Escape) {
		screenshot_cleanup();
	}
}

void
screenshot_cancel(void)
{
	screenshot_cleanup();
}
