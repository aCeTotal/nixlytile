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
	void *src_data;
	uint32_t src_format, copy_w, copy_h, y;
	size_t src_stride;
	uint8_t *dst;

	if (!wlr_buffer_begin_data_ptr_access(&surface->buffer->base,
			WLR_BUFFER_DATA_PTR_ACCESS_READ,
			&src_data, &src_format, &src_stride)) {
		struct wlr_texture *tex = wlr_texture_from_buffer(drw,
			&surface->buffer->base);
		if (!tex)
			return;

		copy_w = MIN((uint32_t)tex->width, cpu_cursor_buf->width);
		copy_h = MIN((uint32_t)tex->height, cpu_cursor_buf->height);

		memset(cpu_cursor_buf->map, 0, cpu_cursor_buf->map_size);

		if (!wlr_texture_read_pixels(tex,
				&(struct wlr_texture_read_pixels_options){
			.data = cpu_cursor_buf->map,
			.format = DRM_FORMAT_ARGB8888,
			.stride = cpu_cursor_buf->stride,
			.src_box = { .width = copy_w, .height = copy_h },
		})) {
			wlr_texture_destroy(tex);
			return;
		}

		wlr_texture_destroy(tex);
	} else {
		memset(cpu_cursor_buf->map, 0, cpu_cursor_buf->map_size);

		copy_w = MIN((uint32_t)surface->current.width, cpu_cursor_buf->width);
		copy_h = MIN((uint32_t)surface->current.height, cpu_cursor_buf->height);

		dst = (uint8_t *)cpu_cursor_buf->map;

		for (y = 0; y < copy_h; y++) {
			memcpy(dst + y * cpu_cursor_buf->stride,
				(uint8_t *)src_data + y * src_stride,
				copy_w * 4);
		}

		wlr_buffer_end_data_ptr_access(&surface->buffer->base);
	}

	wlr_cursor_set_buffer(cursor, &cpu_cursor_buf->base,
		(int32_t)hx, (int32_t)hy, 1.0f);
}

static void
tracked_cursor_handle_destroy(struct wl_listener *listener, void *data)
{
	(void)listener; (void)data;
	stop_tracking_cursor_surface();
}

static void
tracked_cursor_handle_commit(struct wl_listener *listener, void *data)
{
	(void)listener; (void)data;
	if (!tracked_cursor_surface || !tracked_cursor_surface->buffer)
		return;
	tracked_cursor_hx -= tracked_cursor_surface->current.dx;
	tracked_cursor_hy -= tracked_cursor_surface->current.dy;
	upload_cursor_surface(tracked_cursor_surface,
		tracked_cursor_hx, tracked_cursor_hy);
}

void
nixly_cursor_set_xcursor(const char *name)
{
	struct wlr_xcursor *xcur;
	struct wlr_xcursor_image *img;
	uint32_t src_stride, copy_w, copy_h, y;
	const uint8_t *src;
	uint8_t *dst;

	if (!cursor_from_client && cursor_cached_name[0] &&
	    strcmp(cursor_cached_name, name) == 0)
		return;

	cursor_from_client = 0;
	snprintf(cursor_cached_name, sizeof(cursor_cached_name), "%s", name);

	stop_tracking_cursor_surface();

	if (!cpu_cursor_active) {
		wlr_cursor_set_xcursor(cursor, cursor_mgr, name);
		return;
	}

	wlr_xcursor_manager_load(cursor_mgr, 1);
	xcur = wlr_xcursor_manager_get_xcursor(cursor_mgr, name, 1);
	if (!xcur || xcur->image_count == 0) {
		wlr_cursor_set_xcursor(cursor, cursor_mgr, name);
		return;
	}

	img = xcur->images[0];

	memset(cpu_cursor_buf->map, 0, cpu_cursor_buf->map_size);

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
	cursor_from_client = 1;
	cursor_cached_name[0] = '\0';

	stop_tracking_cursor_surface();

	if (!cpu_cursor_active) {
		wlr_cursor_set_surface(cursor, surface, hx, hy);
		return;
	}

	if (!surface) {
		wlr_cursor_set_buffer(cursor, NULL, 0, 0, 1.0f);
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
}
