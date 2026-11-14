#ifndef PLATFORM_BUFFER_BUFFER_H
#define PLATFORM_BUFFER_BUFFER_H

#include <pixman.h>
#include <wayland-server-core.h>
#include "platform/render/dmabuf.h"
#include "platform/container/addon.h"

struct nx_buffer;
struct nx_renderer;

struct nx_shm_attributes {
	int fd;
	uint32_t format; // DRM fourcc
	int width, height;
	int stride; // bytes per row
	off_t offset; // of the first pixel
};

/* One bit per impl hook */
enum nx_buffer_cap {
	NX_BUFFER_CAP_DATA_PTR = 1 << 0,
	NX_BUFFER_CAP_DMABUF = 1 << 1,
	NX_BUFFER_CAP_SHM = 1 << 2,
};

/* Producer drops, consumers lock */
struct nx_buffer {
	const struct nx_buffer_impl *impl;

	int width, height;

	bool dropped;
	size_t n_locks;
	bool accessing_data_ptr;

	struct {
		struct wl_signal destroy;
		struct wl_signal release;
	} events;

	struct nx_addon_set addons;
};

void nx_buffer_drop(struct nx_buffer *buffer);
struct nx_buffer *nx_buffer_lock(struct nx_buffer *buffer);
void nx_buffer_unlock(struct nx_buffer *buffer);

/* Valid while the buffer lives */
bool nx_buffer_get_dmabuf(struct nx_buffer *buffer,
	struct nx_dmabuf_attributes *attribs);
bool nx_buffer_get_shm(struct nx_buffer *buffer,
	struct nx_shm_attributes *attribs);

/* DRM_FORMAT_INVALID unless shm or DMA-BUF */
uint32_t buffer_get_drm_format(struct nx_buffer *buffer);

/* Locked, NULL for non-buffer resources */
struct nx_buffer *nx_buffer_try_from_resource(struct wl_resource *resource);

/* False may still be opaque */
bool nx_buffer_is_opaque(struct nx_buffer *buffer);

enum nx_buffer_data_ptr_access_flag {
	NX_BUFFER_DATA_PTR_ACCESS_READ = 1 << 0,
	NX_BUFFER_DATA_PTR_ACCESS_WRITE = 1 << 1,
};

/* Valid until matching end call */
bool nx_buffer_begin_data_ptr_access(struct nx_buffer *buffer, uint32_t flags,
	void **data, uint32_t *format, size_t *stride);
void nx_buffer_end_data_ptr_access(struct nx_buffer *buffer);

struct nx_client_buffer {
	struct nx_buffer base;

	struct nx_texture *texture; // NULL after renderer destroy
	struct nx_buffer *source; // NULL once destroyed

	struct {
		struct wl_listener source_destroy;
		struct wl_listener renderer_destroy;

		size_t n_ignore_locks;
	};
};

/* NULL for other buffer types */
struct nx_client_buffer *nx_client_buffer_get(struct nx_buffer *buffer);

struct nx_client_buffer *nx_client_buffer_create(struct nx_buffer *buffer,
	struct nx_renderer *renderer);
/* Fails when shared or immutable */
bool nx_client_buffer_apply_damage(struct nx_client_buffer *client_buffer,
	struct nx_buffer *next, const pixman_region32_t *damage);

/* Read-only wrapper around caller memory */
struct nx_readonly_data_buffer {
	struct nx_buffer base;

	const void *data;
	uint32_t format;
	size_t stride;

	void *saved_data;
};

/* data must outlive readonly_data_buffer_drop() */
struct nx_readonly_data_buffer *readonly_data_buffer_create(uint32_t format,
		size_t stride, uint32_t width, uint32_t height, const void *data);
/* Copies data if still locked */
bool readonly_data_buffer_drop(struct nx_readonly_data_buffer *buffer);

/* Solid color from wp_single_pixel_buffer_v1 */
struct nx_single_pixel_buffer_v1 {
	struct nx_buffer base;

	uint32_t r, g, b, a; // UINT32_MAX is full scale

	struct {
		struct wl_resource *resource;
		struct wl_listener release;

		uint8_t argb8888[4]; // little-endian, for data access
	};
};

/* NULL for other buffer types */
struct nx_single_pixel_buffer_v1 *nx_single_pixel_buffer_v1_try_from_buffer(
	struct nx_buffer *buffer);

#endif
