#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "platform/buffer/buffer_interface.h"
#include "platform/log/log.h"
#include "platform/buffer/buffer.h"

static const struct nx_buffer_impl readonly_data_buffer_impl;

static struct nx_readonly_data_buffer *readonly_data_buffer_from_buffer(
		struct nx_buffer *nx_buffer) {
	assert(nx_buffer->impl == &readonly_data_buffer_impl);
	struct nx_readonly_data_buffer *buffer = wl_container_of(nx_buffer, buffer, base);
	return buffer;
}

static void readonly_data_buffer_destroy(struct nx_buffer *nx_buffer) {
	struct nx_readonly_data_buffer *buffer =
		readonly_data_buffer_from_buffer(nx_buffer);
	nx_buffer_finish(nx_buffer);
	free(buffer->saved_data);
	free(buffer);
}

static bool readonly_data_buffer_begin_data_ptr_access(struct nx_buffer *nx_buffer,
		uint32_t flags, void **data, uint32_t *format, size_t *stride) {
	struct nx_readonly_data_buffer *buffer =
		readonly_data_buffer_from_buffer(nx_buffer);
	if (buffer->data == NULL) {
		return false;
	}
	if (flags & NX_BUFFER_DATA_PTR_ACCESS_WRITE) {
		return false;
	}
	*data = (void *)buffer->data;
	*format = buffer->format;
	*stride = buffer->stride;
	return true;
}

static void readonly_data_buffer_end_data_ptr_access(struct nx_buffer *nx_buffer) {
}

static const struct nx_buffer_impl readonly_data_buffer_impl = {
	.destroy = readonly_data_buffer_destroy,
	.begin_data_ptr_access = readonly_data_buffer_begin_data_ptr_access,
	.end_data_ptr_access = readonly_data_buffer_end_data_ptr_access,
};

struct nx_readonly_data_buffer *readonly_data_buffer_create(uint32_t format,
		size_t stride, uint32_t width, uint32_t height, const void *data) {
	struct nx_readonly_data_buffer *buffer = calloc(1, sizeof(*buffer));
	if (buffer == NULL) {
		return NULL;
	}
	nx_buffer_init(&buffer->base, &readonly_data_buffer_impl, width, height);

	buffer->data = data;
	buffer->format = format;
	buffer->stride = stride;

	return buffer;
}

static bool save_data(struct nx_readonly_data_buffer *buffer) {
	size_t size = buffer->stride * buffer->base.height;
	buffer->saved_data = malloc(size);
	if (buffer->saved_data == NULL) {
		nx_log_errno(NX_ERROR, "Allocation failed");
		// Still locked: fail later accesses
		buffer->data = NULL;
		return false;
	}
	memcpy(buffer->saved_data, buffer->data, size);
	buffer->data = buffer->saved_data;
	return true;
}

bool readonly_data_buffer_drop(struct nx_readonly_data_buffer *buffer) {
	bool ok = buffer->base.n_locks == 0 || save_data(buffer);
	nx_buffer_drop(&buffer->base);
	return ok;
}
