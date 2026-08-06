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
draw_border(struct wlr_scene_tree *parent, int x, int y,
		int w, int h, int thickness, const float color[static 4])
{
	drawrect(parent, x, y, w, thickness, color);
	drawrect(parent, x, y + h - thickness, w, thickness, color);
	drawrect(parent, x, y, thickness, h, color);
	drawrect(parent, x + w - thickness, y, thickness, h, color);
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

	if (drmPrimeHandleToFD(drm_fd, buf->gem_handle,
			DRM_CLOEXEC | DRM_RDWR, &prime_fd) < 0) {
		wlr_log(WLR_ERROR, "CPU cursor: drmPrimeHandleToFD failed: %s",
			strerror(errno));
		goto fail;
	}
	buf->dmabuf_fd = prime_fd;

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

/* Cursor image cache + tracked surface for CPU-cursor mode (Nvidia) */
static int    cursor_from_client;
static char   cursor_cached_name[64];
static struct wlr_surface *tracked_cursor_surface;
static struct wl_listener  tracked_cursor_commit;
static struct wl_listener  tracked_cursor_destroy;
static int                 tracked_cursor_hx, tracked_cursor_hy;
static struct CpuCursorBuffer *cursor_shown_buf;

/*
 * Pick the CPU cursor buffer to paint the next image into.
 *
 * wlr_cursor_set_buffer() returns early when buffer, hotspot and scale are all
 * unchanged, and the DRM backend copies (blits/renders) the cursor buffer at
 * set-time on multi-GPU and on the rendered-cursor path.  Painting a new image
 * into the one buffer we already handed over therefore never reaches the
 * screen: the plane keeps showing the previously committed image.  Proton/wine
 * games hit this constantly — they alternate between a transparent
 * hide-cursor image and a real cursor at the *same* hotspot (usually 0,0), so
 * once the transparent one is committed the pointer stays invisible until
 * something changes the hotspot (e.g. the compositor's own arrow after a
 * workspace switch).
 *
 * Alternating between two buffers makes every update a genuinely new buffer,
 * and keeps us from painting into the buffer the cursor plane is scanning out.
 */
static struct CpuCursorBuffer *
cursor_back_buffer(void)
{
	if (cpu_cursor_buf_b && cursor_shown_buf == cpu_cursor_buf)
		return cpu_cursor_buf_b;
	return cpu_cursor_buf;
}

static void
cursor_commit_buffer(struct CpuCursorBuffer *buf, int32_t hx, int32_t hy)
{
	cursor_shown_buf = buf;
	wlr_cursor_set_buffer(cursor, buf ? &buf->base : NULL, hx, hy, 1.0f);
}

static int cursor_paint_xcursor(const char *name);

static void
stop_tracking_cursor_surface(void)
{
	if (!tracked_cursor_surface)
		return;
	wl_list_remove(&tracked_cursor_commit.link);
	wl_list_remove(&tracked_cursor_destroy.link);
	tracked_cursor_surface = NULL;
}

static void
upload_cursor_surface(struct wlr_surface *surface, int hx, int hy)
{
	struct CpuCursorBuffer *buf = cursor_back_buffer();
	void *src_data;
	uint32_t src_format, copy_w, copy_h, y;
	size_t src_stride;
	uint8_t *dst;

	if (!buf)
		return;

	if (!wlr_buffer_begin_data_ptr_access(&surface->buffer->base,
			WLR_BUFFER_DATA_PTR_ACCESS_READ,
			&src_data, &src_format, &src_stride)) {
		/* The client often destroys the source wl_buffer right after
		 * commit; the wlr_client_buffer then has no dmabuf/shm/data-ptr
		 * left, so importing a fresh texture fails.  The texture created
		 * at commit time survives, so prefer it. */
		struct wlr_texture *tex = wlr_surface_get_texture(surface);
		struct wlr_texture *own_tex = NULL;
		if (!tex) {
			own_tex = wlr_texture_from_buffer(drw,
				&surface->buffer->base);
			tex = own_tex;
		}
		if (!tex) {
			game_log("CURSOR: client surface upload failed "
				"(no data ptr, no texture) — keeping previous image");
			return;
		}

		copy_w = MIN((uint32_t)tex->width, buf->width);
		copy_h = MIN((uint32_t)tex->height, buf->height);

		memset(buf->map, 0, buf->map_size);

		if (!wlr_texture_read_pixels(tex,
				&(struct wlr_texture_read_pixels_options){
			.data = buf->map,
			.format = DRM_FORMAT_ARGB8888,
			.stride = buf->stride,
			.src_box = { .width = copy_w, .height = copy_h },
		})) {
			wlr_texture_destroy(own_tex);
			/* Buffer is blank now, but it is the back buffer — the
			 * displayed image is untouched because we don't commit. */
			game_log("CURSOR: client surface read_pixels failed "
				"(%ux%u) — keeping previous image", copy_w, copy_h);
			return;
		}

		wlr_texture_destroy(own_tex);
	} else {
		memset(buf->map, 0, buf->map_size);

		copy_w = MIN((uint32_t)surface->current.width, buf->width);
		copy_h = MIN((uint32_t)surface->current.height, buf->height);

		dst = (uint8_t *)buf->map;

		for (y = 0; y < copy_h; y++) {
			memcpy(dst + y * buf->stride,
				(uint8_t *)src_data + y * src_stride,
				copy_w * 4);
		}

		wlr_buffer_end_data_ptr_access(&surface->buffer->base);
	}

	{
		uint32_t px, opaque = 0;
		const uint8_t *p = (const uint8_t *)buf->map;
		for (y = 0; y < copy_h; y++)
			for (px = 0; px < copy_w; px++)
				if (p[y * buf->stride + px * 4 + 3])
					opaque++;
		game_log("CURSOR: client image %ux%u hotspot=%d,%d "
			"nonzero_alpha=%u buf=%s", copy_w, copy_h, hx, hy, opaque,
			buf == cpu_cursor_buf ? "A" : "B");
	}

	cursor_commit_buffer(buf, (int32_t)hx, (int32_t)hy);
}

static void
tracked_cursor_handle_destroy(struct wl_listener *listener, void *data)
{
	(void)listener; (void)data;
	stop_tracking_cursor_surface();
}

/* The CPU-cursor path never calls wlr_cursor_set_surface(), which is the
 * only wlroots path that answers wl_surface.frame requests on cursor
 * surfaces (cursor_output_cursor_handle_output_commit).  Xwayland requests
 * a frame callback with every cursor image it attaches and defers ALL
 * further cursor updates until it fires — so without this, an Xwayland
 * game's first show/hide cycle works and every later "show" is silently
 * withheld by Xwayland: the pointer stays invisible for the rest of the
 * session (menus included).  Drain the callbacks ourselves. */
static void
cursor_surface_send_frame_done(struct wlr_surface *surface)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_surface_send_frame_done(surface, &now);
}

static void
tracked_cursor_handle_commit(struct wl_listener *listener, void *data)
{
	(void)listener; (void)data;
	if (!tracked_cursor_surface)
		return;
	if (tracked_cursor_surface->buffer) {
		tracked_cursor_hx -= tracked_cursor_surface->current.dx;
		tracked_cursor_hy -= tracked_cursor_surface->current.dy;
		upload_cursor_surface(tracked_cursor_surface,
			tracked_cursor_hx, tracked_cursor_hy);
	}
	cursor_surface_send_frame_done(tracked_cursor_surface);
}

/* Paint an xcursor image into the CPU back buffer and commit it.  Image
 * only — leaves the client/xcursor cache state and any tracked client
 * cursor surface untouched, so callers decide the ownership semantics.
 * Returns 0 if the xcursor or buffer is unavailable. */
static int
cursor_paint_xcursor(const char *name)
{
	struct CpuCursorBuffer *buf;
	struct wlr_xcursor *xcur;
	struct wlr_xcursor_image *img;
	uint32_t src_stride, copy_w, copy_h, y;
	const uint8_t *src;
	uint8_t *dst;

	wlr_xcursor_manager_load(cursor_mgr, 1);
	xcur = wlr_xcursor_manager_get_xcursor(cursor_mgr, name, 1);
	if (!xcur || xcur->image_count == 0)
		return 0;

	buf = cursor_back_buffer();
	if (!buf)
		return 0;

	img = xcur->images[0];

	memset(buf->map, 0, buf->map_size);

	copy_w = MIN(img->width, buf->width);
	copy_h = MIN(img->height, buf->height);
	src_stride = img->width * 4;

	src = img->buffer;
	dst = (uint8_t *)buf->map;

	for (y = 0; y < copy_h; y++) {
		memcpy(dst + y * buf->stride,
			src + y * src_stride,
			copy_w * 4);
	}

	game_log("CURSOR: xcursor '%s' %ux%u hotspot=%u,%u buf=%s", name,
		copy_w, copy_h, img->hotspot_x, img->hotspot_y,
		buf == cpu_cursor_buf ? "A" : "B");

	cursor_commit_buffer(buf, (int32_t)img->hotspot_x, (int32_t)img->hotspot_y);
	return 1;
}

void
nixly_cursor_set_xcursor(const char *name)
{
	if (!cursor_from_client && cursor_cached_name[0] &&
	    strcmp(cursor_cached_name, name) == 0)
		return;

	cursor_from_client = 0;
	snprintf(cursor_cached_name, sizeof(cursor_cached_name), "%s", name);

	stop_tracking_cursor_surface();

	if (!cpu_cursor_active || !cursor_paint_xcursor(name))
		wlr_cursor_set_xcursor(cursor, cursor_mgr, name);
}

void
nixly_cursor_set_client_surface(struct wlr_surface *surface, int hx, int hy)
{
	cursor_from_client = 1;
	cursor_cached_name[0] = '\0';

	stop_tracking_cursor_surface();

	if (!cpu_cursor_active) {
		wlr_cursor_set_surface(cursor, surface, hx, hy);
		return;
	}

	if (!surface) {
		game_log("CURSOR: client hid cursor (NULL surface)");
		cursor_commit_buffer(NULL, 0, 0);
		return;
	}

	tracked_cursor_surface = surface;
	tracked_cursor_hx = hx;
	tracked_cursor_hy = hy;
	tracked_cursor_commit.notify = tracked_cursor_handle_commit;
	tracked_cursor_destroy.notify = tracked_cursor_handle_destroy;
	wl_signal_add(&surface->events.commit, &tracked_cursor_commit);
	wl_signal_add(&surface->events.destroy, &tracked_cursor_destroy);

	if (surface->buffer) {
		upload_cursor_surface(surface, hx, hy);
	}
	/* Drain any frame callback committed before this set_cursor arrived
	 * (clients may commit the cursor surface first).  No-op when the
	 * callback list is empty. */
	cursor_surface_send_frame_done(surface);
}
