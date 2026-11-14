#ifndef PLATFORM_BUFFER_BUFFER_INTERFACE_H
#define PLATFORM_BUFFER_BUFFER_INTERFACE_H

#include "platform/buffer/buffer.h"

struct nx_buffer_impl {
	void (*destroy)(struct nx_buffer *buffer);
	bool (*get_dmabuf)(struct nx_buffer *buffer,
		struct nx_dmabuf_attributes *attribs);
	bool (*get_shm)(struct nx_buffer *buffer,
		struct nx_shm_attributes *attribs);
	bool (*begin_data_ptr_access)(struct nx_buffer *buffer, uint32_t flags,
		void **data, uint32_t *format, size_t *stride);
	void (*end_data_ptr_access)(struct nx_buffer *buffer);
};

struct nx_buffer_resource_interface {
	const char *name;
	bool (*is_instance)(struct wl_resource *resource);
	struct nx_buffer *(*from_resource)(struct wl_resource *resource);
};

/* Starts referenced by the producer */
void nx_buffer_init(struct nx_buffer *buffer,
	const struct nx_buffer_impl *impl, int width, int height);

/* Emits destroy, frees common state */
void nx_buffer_finish(struct nx_buffer *buffer);

/* Registers a wl_buffer type */
void nx_buffer_register_resource_interface(
	const struct nx_buffer_resource_interface *iface);

#endif
