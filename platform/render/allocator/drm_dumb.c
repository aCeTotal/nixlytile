#include <assert.h>
#include <drm_fourcc.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include "platform/buffer/buffer_interface.h"
#include "platform/log/log.h"
#include "platform/render/allocator/allocator.h"
#include "platform/render/allocator/drm_dumb.h"
#include "platform/render/dmabuf.h"
#include "platform/render/drm_format_set.h"
#include "platform/render/pixel_format.h"

struct nx_drm_dumb_buffer {
	struct nx_buffer base;
	struct wl_list link; // nx_drm_dumb_allocator.buffers

	int drm_fd; // -1 after allocator destroy
	struct nx_dmabuf_attributes dmabuf;

	uint32_t format;
	uint32_t handle;
	uint32_t stride;
	uint32_t width, height;

	uint64_t size;
	void *data;
};

struct nx_drm_dumb_allocator {
	struct nx_allocator base;
	struct wl_list buffers; // nx_drm_dumb_buffer.link
	int drm_fd;
};

static const struct nx_buffer_impl buffer_impl;

static struct nx_drm_dumb_buffer *drm_dumb_buffer_from_buffer(
		struct nx_buffer *nx_buf) {
	assert(nx_buf->impl == &buffer_impl);
	struct nx_drm_dumb_buffer *buf = wl_container_of(nx_buf, buf, base);
	return buf;
}

static struct nx_drm_dumb_buffer *create_buffer(
		struct nx_drm_dumb_allocator *alloc, int width, int height,
		const struct nx_drm_format *format) {
	if (!nx_drm_format_has(format, DRM_FORMAT_MOD_INVALID) &&
			!nx_drm_format_has(format, DRM_FORMAT_MOD_LINEAR)) {
		nx_log(NX_ERROR, "DRM dumb allocator only supports INVALID and "
			"LINEAR modifiers");
		return NULL;
	}

	const struct nx_pixel_format_info *info =
		drm_get_pixel_format_info(format->format);
	if (info == NULL) {
		nx_log(NX_ERROR, "DRM format 0x%"PRIX32" not supported",
			format->format);
		return NULL;
	}
	if (pixel_format_info_pixels_per_block(info) != 1) {
		nx_log(NX_ERROR, "Block formats are not supported");
		return NULL;
	}

	struct nx_drm_dumb_buffer *buffer = calloc(1, sizeof(*buffer));
	if (buffer == NULL) {
		return NULL;
	}
	nx_buffer_init(&buffer->base, &buffer_impl, width, height);
	wl_list_insert(&alloc->buffers, &buffer->link);

	buffer->drm_fd = alloc->drm_fd;

	uint32_t bpp = 8 * info->bytes_per_block;
	if (drmModeCreateDumbBuffer(alloc->drm_fd, width, height, bpp, 0,
			&buffer->handle, &buffer->stride, &buffer->size) != 0) {
		nx_log_errno(NX_ERROR, "Failed to create DRM dumb buffer");
		goto create_destroy;
	}

	buffer->width = width;
	buffer->height = height;
	buffer->format = format->format;

	uint64_t offset;
	if (drmModeMapDumbBuffer(alloc->drm_fd, buffer->handle, &offset) != 0) {
		nx_log_errno(NX_ERROR, "Failed to map DRM dumb buffer");
		goto create_destroy;
	}

	buffer->data = mmap(NULL, buffer->size, PROT_READ | PROT_WRITE, MAP_SHARED,
		alloc->drm_fd, offset);
	if (buffer->data == MAP_FAILED) {
		nx_log_errno(NX_ERROR, "Failed to mmap DRM dumb buffer");
		goto create_destroy;
	}

	memset(buffer->data, 0, buffer->size);

	int prime_fd;
	if (drmPrimeHandleToFD(alloc->drm_fd, buffer->handle, DRM_CLOEXEC,
			&prime_fd) != 0) {
		nx_log_errno(NX_ERROR, "Failed to get PRIME handle from GEM handle");
		goto create_destroy;
	}

	buffer->dmabuf = (struct nx_dmabuf_attributes){
		.width = buffer->width,
		.height = buffer->height,
		.format = format->format,
		.modifier = DRM_FORMAT_MOD_LINEAR,
		.n_planes = 1,
		.offset[0] = 0,
		.stride[0] = buffer->stride,
		.fd[0] = prime_fd,
	};

	nx_log(NX_DEBUG, "Allocated %"PRIu32"x%"PRIu32" DRM dumb buffer",
			buffer->width, buffer->height);

	return buffer;

create_destroy:
	nx_buffer_drop(&buffer->base);
	return NULL;
}

static bool drm_dumb_buffer_begin_data_ptr_access(struct nx_buffer *nx_buffer,
		uint32_t flags, void **data, uint32_t *format, size_t *stride) {
	struct nx_drm_dumb_buffer *buf = drm_dumb_buffer_from_buffer(nx_buffer);
	*data = buf->data;
	*stride = buf->stride;
	*format = buf->format;
	return true;
}

static void drm_dumb_buffer_end_data_ptr_access(struct nx_buffer *nx_buffer) {
}

static bool buffer_get_dmabuf(struct nx_buffer *nx_buffer,
		struct nx_dmabuf_attributes *attribs) {
	struct nx_drm_dumb_buffer *buf = drm_dumb_buffer_from_buffer(nx_buffer);
	*attribs = buf->dmabuf;
	return true;
}

static void buffer_destroy(struct nx_buffer *nx_buffer) {
	struct nx_drm_dumb_buffer *buf = drm_dumb_buffer_from_buffer(nx_buffer);

	nx_buffer_finish(nx_buffer);

	if (buf->data) {
		munmap(buf->data, buf->size);
	}

	nx_dmabuf_attributes_finish(&buf->dmabuf);

	if (buf->drm_fd >= 0 && drmModeDestroyDumbBuffer(buf->drm_fd, buf->handle) != 0) {
		nx_log_errno(NX_ERROR, "Failed to destroy DRM dumb buffer");
	}

	wl_list_remove(&buf->link);
	free(buf);
}

static const struct nx_buffer_impl buffer_impl = {
	.destroy = buffer_destroy,
	.get_dmabuf = buffer_get_dmabuf,
	.begin_data_ptr_access = drm_dumb_buffer_begin_data_ptr_access,
	.end_data_ptr_access = drm_dumb_buffer_end_data_ptr_access,
};

static const struct nx_allocator_interface allocator_impl;

static struct nx_drm_dumb_allocator *drm_dumb_alloc_from_alloc(
		struct nx_allocator *nx_alloc) {
	assert(nx_alloc->impl == &allocator_impl);
	struct nx_drm_dumb_allocator *alloc = wl_container_of(nx_alloc, alloc, base);
	return alloc;
}

static struct nx_buffer *allocator_create_buffer(
		struct nx_allocator *nx_alloc, int width, int height,
		const struct nx_drm_format *drm_format) {
	struct nx_drm_dumb_allocator *alloc = drm_dumb_alloc_from_alloc(nx_alloc);
	struct nx_drm_dumb_buffer *buffer = create_buffer(alloc, width, height,
			drm_format);
	if (buffer == NULL) {
		return NULL;
	}
	return &buffer->base;
}

static void allocator_destroy(struct nx_allocator *nx_alloc) {
	struct nx_drm_dumb_allocator *alloc = drm_dumb_alloc_from_alloc(nx_alloc);

	struct nx_drm_dumb_buffer *buf, *buf_tmp;
	wl_list_for_each_safe(buf, buf_tmp, &alloc->buffers, link) {
		buf->drm_fd = -1;
		wl_list_remove(&buf->link);
		wl_list_init(&buf->link);
	}

	close(alloc->drm_fd);
	free(alloc);
}

static const struct nx_allocator_interface allocator_impl = {
	.create_buffer = allocator_create_buffer,
	.destroy = allocator_destroy,
};

struct nx_allocator *nx_drm_dumb_allocator_create(int drm_fd) {
	if (drmGetNodeTypeFromFd(drm_fd) != DRM_NODE_PRIMARY) {
		nx_log(NX_ERROR, "Cannot use DRM dumb buffers with non-primary DRM FD");
		return NULL;
	}

	uint64_t has_dumb = 0;
	if (drmGetCap(drm_fd, DRM_CAP_DUMB_BUFFER, &has_dumb) < 0) {
		nx_log(NX_ERROR, "Failed to get DRM capabilities");
		return NULL;
	}

	if (has_dumb == 0) {
		nx_log(NX_ERROR, "DRM dumb buffers not supported");
		return NULL;
	}

	struct nx_drm_dumb_allocator *alloc = calloc(1, sizeof(*alloc));
	if (alloc == NULL) {
		return NULL;
	}
	nx_allocator_init(&alloc->base, &allocator_impl,
		NX_BUFFER_CAP_DATA_PTR | NX_BUFFER_CAP_DMABUF);

	alloc->drm_fd = drm_fd;
	wl_list_init(&alloc->buffers);

	nx_log(NX_DEBUG, "Created DRM dumb allocator");
	return &alloc->base;
}
