#include <assert.h>
#include <drm_fourcc.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
#include "platform/buffer/buffer_interface.h"
#include "platform/log/log.h"
#include "platform/os/shm.h"
#include "platform/render/allocator/allocator.h"
#include "platform/render/allocator/shm.h"
#include "platform/render/drm_format_set.h"
#include "platform/render/pixel_format.h"

struct nx_mem_buffer {
	struct nx_buffer base;
	struct nx_shm_attributes shm;
	void *data;
	size_t size;
};

static const struct nx_buffer_impl buffer_impl;

static struct nx_mem_buffer *mem_buffer_from_buffer(
		struct nx_buffer *nx_buffer) {
	assert(nx_buffer->impl == &buffer_impl);
	struct nx_mem_buffer *buffer = wl_container_of(nx_buffer, buffer, base);
	return buffer;
}

static void buffer_destroy(struct nx_buffer *nx_buffer) {
	struct nx_mem_buffer *buffer = mem_buffer_from_buffer(nx_buffer);
	nx_buffer_finish(nx_buffer);
	munmap(buffer->data, buffer->size);
	close(buffer->shm.fd);
	free(buffer);
}

static bool buffer_get_shm(struct nx_buffer *nx_buffer,
		struct nx_shm_attributes *shm) {
	struct nx_mem_buffer *buffer = mem_buffer_from_buffer(nx_buffer);
	*shm = buffer->shm;
	return true;
}

static bool shm_buffer_begin_data_ptr_access(struct nx_buffer *nx_buffer,
		uint32_t flags, void **data, uint32_t *format, size_t *stride) {
	struct nx_mem_buffer *buffer = mem_buffer_from_buffer(nx_buffer);
	*data = buffer->data;
	*format = buffer->shm.format;
	*stride = buffer->shm.stride;
	return true;
}

static void shm_buffer_end_data_ptr_access(struct nx_buffer *nx_buffer) {
}

static const struct nx_buffer_impl buffer_impl = {
	.destroy = buffer_destroy,
	.get_shm = buffer_get_shm,
	.begin_data_ptr_access = shm_buffer_begin_data_ptr_access,
	.end_data_ptr_access = shm_buffer_end_data_ptr_access,
};

static struct nx_buffer *allocator_create_buffer(
		struct nx_allocator *nx_allocator, int width, int height,
		const struct nx_drm_format *format) {
	const struct nx_pixel_format_info *info =
		drm_get_pixel_format_info(format->format);
	if (info == NULL) {
		nx_log(NX_ERROR, "Unsupported pixel format 0x%"PRIX32, format->format);
		return NULL;
	}

	struct nx_mem_buffer *buffer = calloc(1, sizeof(*buffer));
	if (buffer == NULL) {
		return NULL;
	}
	nx_buffer_init(&buffer->base, &buffer_impl, width, height);

	int stride = pixel_format_info_min_stride(info, width);
	buffer->size = stride * height;
	buffer->shm.fd = allocate_shm_file(buffer->size);
	if (buffer->shm.fd < 0) {
		free(buffer);
		return NULL;
	}

	buffer->shm.format = format->format;
	buffer->shm.width = width;
	buffer->shm.height = height;
	buffer->shm.stride = stride;
	buffer->shm.offset = 0;

	buffer->data = mmap(NULL, buffer->size, PROT_READ | PROT_WRITE, MAP_SHARED,
		buffer->shm.fd, 0);
	if (buffer->data == MAP_FAILED) {
		nx_log_errno(NX_ERROR, "mmap failed");
		close(buffer->shm.fd);
		free(buffer);
		return NULL;
	}

	return &buffer->base;
}

static void allocator_destroy(struct nx_allocator *nx_allocator) {
	free(nx_allocator);
}

static const struct nx_allocator_interface allocator_impl = {
	.destroy = allocator_destroy,
	.create_buffer = allocator_create_buffer,
};

struct nx_allocator *nx_shm_allocator_create(void) {
	struct nx_allocator *allocator = calloc(1, sizeof(*allocator));
	if (allocator == NULL) {
		return NULL;
	}
	nx_allocator_init(allocator, &allocator_impl,
		NX_BUFFER_CAP_DATA_PTR | NX_BUFFER_CAP_SHM);

	nx_log(NX_DEBUG, "Created shm allocator");
	return allocator;
}
