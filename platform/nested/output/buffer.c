#include <drm_fourcc.h>
#include <stdlib.h>
#include "platform/buffer/buffer.h"
#include "platform/log/log.h"
#include "platform/nested/output/output.h"
#include "platform/render/pixel_format.h"

#include "linux-dmabuf-v1-client-protocol.h"

static void buffer_remove_drm_syncobj_waiter(struct nx_wl_buffer *buffer) {
	nx_drm_syncobj_timeline_waiter_finish(&buffer->drm_syncobj_waiter);
	buffer->has_drm_syncobj_waiter = false;
}

static void buffer_destroy(struct nx_wl_buffer *buffer) {
	wl_list_remove(&buffer->buffer_destroy.link);
	wl_list_remove(&buffer->link);
	wl_buffer_destroy(buffer->wl_buffer);
	if (buffer->has_drm_syncobj_waiter) {
		buffer_remove_drm_syncobj_waiter(buffer);
	}
	if (!buffer->released) {
		nx_buffer_unlock(buffer->buffer);
	}
	nx_drm_syncobj_timeline_unref(buffer->fallback_signal_timeline);
	free(buffer);
}

static void buffer_release(struct nx_wl_buffer *buffer) {
	if (buffer->released) {
		return;
	}
	buffer->released = true;
	// May free the buffer.
	nx_buffer_unlock(buffer->buffer);
}

// Explicit sync releases through the timeline.
static void buffer_handle_release(void *data, struct wl_buffer *wl_buffer) {
	struct nx_wl_buffer *buffer = data;
	if (buffer->has_drm_syncobj_waiter) {
		return;
	}
	buffer_release(buffer);
}

static const struct wl_buffer_listener buffer_listener = {
	.release = buffer_handle_release,
};

void nested_buffer_sync_ready(struct nx_drm_syncobj_timeline_waiter *waiter) {
	struct nx_wl_buffer *buffer = wl_container_of(waiter, buffer, drm_syncobj_waiter);
	buffer_remove_drm_syncobj_waiter(buffer);
	buffer_release(buffer);
}

static void buffer_handle_buffer_destroy(struct wl_listener *listener,
		void *data) {
	struct nx_wl_buffer *buffer =
		wl_container_of(listener, buffer, buffer_destroy);
	buffer_destroy(buffer);
}

bool nested_buffer_test(struct nx_wl_backend *wl, struct nx_buffer *nx_buffer) {
	struct nx_dmabuf_attributes dmabuf;
	if (nx_buffer_get_dmabuf(nx_buffer, &dmabuf)) {
		return nx_drm_format_set_has(&wl->linux_dmabuf_v1_formats,
			dmabuf.format, dmabuf.modifier);
	}
	struct nx_shm_attributes shm;
	if (nx_buffer_get_shm(nx_buffer, &shm)) {
		return nx_drm_format_set_has(&wl->shm_formats, shm.format,
			DRM_FORMAT_MOD_INVALID);
	}
	return false;
}

struct dmabuf_import {
	struct wl_buffer *wl_buffer;
	bool done;
};

static void dmabuf_handle_created(void *data, struct zwp_linux_buffer_params_v1 *params,
		struct wl_buffer *buffer) {
	struct dmabuf_import *import = data;
	import->wl_buffer = buffer;
	import->done = true;
	nx_log(NX_DEBUG, "DMA-BUF imported into parent Wayland compositor");
}

static void dmabuf_handle_failed(void *data, struct zwp_linux_buffer_params_v1 *params) {
	struct dmabuf_import *import = data;
	import->done = true;
	nx_log(NX_ERROR, "Failed to import DMA-BUF into parent Wayland compositor");
}

static const struct zwp_linux_buffer_params_v1_listener dmabuf_listener = {
	.created = dmabuf_handle_created,
	.failed = dmabuf_handle_failed,
};

static struct wl_buffer *import_dmabuf(struct nx_wl_backend *wl,
		const struct nx_dmabuf_attributes *dmabuf) {
	uint32_t modifier_hi = dmabuf->modifier >> 32;
	uint32_t modifier_lo = (uint32_t)dmabuf->modifier;
	struct zwp_linux_buffer_params_v1 *params =
		zwp_linux_dmabuf_v1_create_params(wl->zwp_linux_dmabuf_v1);
	for (int i = 0; i < dmabuf->n_planes; i++) {
		zwp_linux_buffer_params_v1_add(params, dmabuf->fd[i], i,
			dmabuf->offset[i], dmabuf->stride[i], modifier_hi, modifier_lo);
	}

	struct dmabuf_import import = {0};
	zwp_linux_buffer_params_v1_add_listener(params, &dmabuf_listener, &import);
	zwp_linux_buffer_params_v1_create(params, dmabuf->width, dmabuf->height,
		dmabuf->format, 0);

	// Waits on a private queue for the reply.
	struct wl_event_queue *display_queue =
		wl_proxy_get_queue((struct wl_proxy *)wl->remote_display);
	wl_proxy_set_queue((struct wl_proxy *)params, wl->busy_loop_queue);
	while (!import.done) {
		if (wl_display_dispatch_queue(wl->remote_display, wl->busy_loop_queue) < 0) {
			nx_log(NX_ERROR, "wl_display_dispatch_queue() failed");
			break;
		}
	}

	if (import.wl_buffer != NULL) {
		wl_proxy_set_queue((struct wl_proxy *)import.wl_buffer, display_queue);
	}
	zwp_linux_buffer_params_v1_destroy(params);
	return import.wl_buffer;
}

static struct wl_buffer *import_shm(struct nx_wl_backend *wl,
		const struct nx_shm_attributes *shm) {
	enum wl_shm_format wl_shm_format = convert_drm_format_to_wl_shm(shm->format);
	uint32_t size = shm->stride * shm->height;
	struct wl_shm_pool *pool = wl_shm_create_pool(wl->shm, shm->fd, shm->offset + size);
	if (pool == NULL) {
		return NULL;
	}
	struct wl_buffer *wl_buffer = wl_shm_pool_create_buffer(pool, shm->offset,
		shm->width, shm->height, shm->stride, wl_shm_format);
	wl_shm_pool_destroy(pool);
	return wl_buffer;
}

static struct wl_buffer *import_buffer(struct nx_wl_backend *wl,
		struct nx_buffer *nx_buffer) {
	struct nx_dmabuf_attributes dmabuf;
	if (nx_buffer_get_dmabuf(nx_buffer, &dmabuf)) {
		return import_dmabuf(wl, &dmabuf);
	}
	struct nx_shm_attributes shm;
	if (nx_buffer_get_shm(nx_buffer, &shm)) {
		return import_shm(wl, &shm);
	}
	return NULL;
}

static struct nx_wl_buffer *create_wl_buffer(struct nx_wl_backend *wl,
		struct nx_buffer *nx_buffer) {
	if (!nested_buffer_test(wl, nx_buffer)) {
		return NULL;
	}
	struct wl_buffer *wl_buffer = import_buffer(wl, nx_buffer);
	if (wl_buffer == NULL) {
		return NULL;
	}

	struct nx_wl_buffer *buffer = calloc(1, sizeof(*buffer));
	if (buffer == NULL) {
		wl_buffer_destroy(wl_buffer);
		return NULL;
	}
	buffer->wl_buffer = wl_buffer;
	buffer->buffer = nx_buffer_lock(nx_buffer);
	wl_list_insert(&wl->buffers, &buffer->link);

	wl_buffer_add_listener(wl_buffer, &buffer_listener, buffer);

	buffer->buffer_destroy.notify = buffer_handle_buffer_destroy;
	wl_signal_add(&nx_buffer->events.destroy, &buffer->buffer_destroy);
	return buffer;
}

struct nx_wl_buffer *nested_buffer_get_or_create(struct nx_wl_backend *wl,
		struct nx_buffer *nx_buffer) {
	struct nx_wl_buffer *buffer;
	wl_list_for_each(buffer, &wl->buffers, link) {
		// wl_buffer.release is per buffer, not per commit.
		if (buffer->buffer == nx_buffer && buffer->released) {
			buffer->released = false;
			nx_buffer_lock(buffer->buffer);
			return buffer;
		}
	}
	return create_wl_buffer(wl, nx_buffer);
}

void nested_buffers_destroy(struct nx_wl_backend *wl) {
	// Destroying one may destroy the next.
	while (!wl_list_empty(&wl->buffers)) {
		struct nx_wl_buffer *buffer = wl_container_of(wl->buffers.next, buffer, link);
		buffer_destroy(buffer);
	}
}
