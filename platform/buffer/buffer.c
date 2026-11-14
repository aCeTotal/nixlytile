#include <assert.h>
#include <drm_fourcc.h>
#include "platform/buffer/buffer_interface.h"
#include "platform/render/pixel_format.h"
#include "platform/buffer/buffer.h"

void nx_buffer_init(struct nx_buffer *buffer,
		const struct nx_buffer_impl *impl, int width, int height) {
	assert(impl->destroy);
	assert(!impl->begin_data_ptr_access == !impl->end_data_ptr_access);

	*buffer = (struct nx_buffer){
		.impl = impl,
		.width = width,
		.height = height,
	};

	wl_signal_init(&buffer->events.destroy);
	wl_signal_init(&buffer->events.release);

	nx_addon_set_init(&buffer->addons);
}

void nx_buffer_finish(struct nx_buffer *buffer) {
	wl_signal_emit_mutable(&buffer->events.destroy, NULL);
	nx_addon_set_finish(&buffer->addons);

	assert(wl_list_empty(&buffer->events.destroy.listener_list));
	assert(wl_list_empty(&buffer->events.release.listener_list));
}

static void buffer_consider_destroy(struct nx_buffer *buffer) {
	if (!buffer->dropped || buffer->n_locks > 0) {
		return;
	}

	assert(!buffer->accessing_data_ptr);

	buffer->impl->destroy(buffer);
}

void nx_buffer_drop(struct nx_buffer *buffer) {
	if (buffer == NULL) {
		return;
	}

	assert(!buffer->dropped);
	buffer->dropped = true;
	buffer_consider_destroy(buffer);
}

struct nx_buffer *nx_buffer_lock(struct nx_buffer *buffer) {
	buffer->n_locks++;
	return buffer;
}

void nx_buffer_unlock(struct nx_buffer *buffer) {
	if (buffer == NULL) {
		return;
	}

	assert(buffer->n_locks > 0);
	buffer->n_locks--;

	if (buffer->n_locks == 0) {
		wl_signal_emit_mutable(&buffer->events.release, NULL);
	}

	buffer_consider_destroy(buffer);
}

bool nx_buffer_get_dmabuf(struct nx_buffer *buffer,
		struct nx_dmabuf_attributes *attribs) {
	if (!buffer->impl->get_dmabuf) {
		return false;
	}
	return buffer->impl->get_dmabuf(buffer, attribs);
}

bool nx_buffer_begin_data_ptr_access(struct nx_buffer *buffer, uint32_t flags,
		void **data, uint32_t *format, size_t *stride) {
	assert(!buffer->accessing_data_ptr);
	if (!buffer->impl->begin_data_ptr_access) {
		return false;
	}
	if (!buffer->impl->begin_data_ptr_access(buffer, flags, data, format, stride)) {
		return false;
	}
	buffer->accessing_data_ptr = true;
	return true;
}

void nx_buffer_end_data_ptr_access(struct nx_buffer *buffer) {
	assert(buffer->accessing_data_ptr);
	buffer->impl->end_data_ptr_access(buffer);
	buffer->accessing_data_ptr = false;
}

bool nx_buffer_get_shm(struct nx_buffer *buffer,
		struct nx_shm_attributes *attribs) {
	if (!buffer->impl->get_shm) {
		return false;
	}
	return buffer->impl->get_shm(buffer, attribs);
}

bool nx_buffer_is_opaque(struct nx_buffer *buffer) {
	uint32_t format = buffer_get_drm_format(buffer);
	if (format != DRM_FORMAT_INVALID) {
		return !pixel_format_has_alpha(format);
	}

	void *data;
	size_t stride;
	if (!nx_buffer_begin_data_ptr_access(buffer,
			NX_BUFFER_DATA_PTR_ACCESS_READ, &data, &format, &stride)) {
		return false;
	}
	const uint8_t *argb8888 = data;
	bool opaque = buffer->width == 1 && buffer->height == 1 &&
		format == DRM_FORMAT_ARGB8888 && argb8888[3] == 0xFF;
	nx_buffer_end_data_ptr_access(buffer);

	return opaque || !pixel_format_has_alpha(format);
}

uint32_t buffer_get_drm_format(struct nx_buffer *buffer) {
	struct nx_dmabuf_attributes dmabuf;
	if (nx_buffer_get_dmabuf(buffer, &dmabuf)) {
		return dmabuf.format;
	}
	struct nx_shm_attributes shm;
	if (nx_buffer_get_shm(buffer, &shm)) {
		return shm.format;
	}
	return DRM_FORMAT_INVALID;
}
