#include <assert.h>
#include <stdlib.h>
#include <unistd.h>
#include <wayland-server-protocol.h>
#include "platform/buffer/buffer_interface.h"
#include "platform/buffer/shm/mapping.h"
#include "platform/buffer/shm/pool.h"
#include "platform/render/pixel_format.h"

struct nx_shm_pool {
	struct wl_resource *resource; // NULL once destroyed
	struct nx_shm *shm;
	struct wl_list buffers; // nx_shm_buffer.link
	int fd;
	struct nx_shm_mapping *mapping;
};

struct nx_shm_buffer {
	struct nx_buffer base;
	struct nx_shm_pool *pool;
	uint32_t drm_format;
	int32_t stride;
	off_t offset;
	struct wl_list link; // nx_shm_pool.buffers
	struct wl_resource *resource; // NULL once destroyed

	struct wl_listener release;

	struct nx_shm_sigbus_data sigbus_data;
};

static const struct wl_buffer_interface wl_buffer_impl;
static const struct wl_shm_pool_interface pool_impl;

static bool buffer_resource_is_instance(struct wl_resource *resource) {
	return wl_resource_instance_of(resource, &wl_buffer_interface,
		&wl_buffer_impl);
}

static struct nx_shm_buffer *buffer_from_resource(struct wl_resource *resource) {
	assert(buffer_resource_is_instance(resource));
	return wl_resource_get_user_data(resource);
}

static struct nx_buffer *buffer_base_from_resource(struct wl_resource *resource) {
	return &buffer_from_resource(resource)->base;
}

const struct nx_buffer_resource_interface nx_shm_buffer_resource_interface = {
	.name = "wl_shm",
	.is_instance = buffer_resource_is_instance,
	.from_resource = buffer_base_from_resource,
};

static struct nx_shm_pool *pool_from_resource(struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &wl_shm_pool_interface,
		&pool_impl));
	return wl_resource_get_user_data(resource);
}

static bool shm_has_format(struct nx_shm *shm, uint32_t shm_format) {
	for (size_t i = 0; i < shm->formats_len; i++) {
		if (shm->formats[i] == shm_format) {
			return true;
		}
	}
	return false;
}

static void pool_consider_destroy(struct nx_shm_pool *pool) {
	if (pool->resource != NULL || !wl_list_empty(&pool->buffers)) {
		return;
	}

	nx_shm_mapping_drop(pool->mapping);
	close(pool->fd);
	free(pool);
}

static void buffer_destroy(struct nx_buffer *nx_buffer) {
	struct nx_shm_buffer *buffer = wl_container_of(nx_buffer, buffer, base);
	wl_list_remove(&buffer->release.link);

	nx_buffer_finish(nx_buffer);

	assert(buffer->resource == NULL);
	wl_list_remove(&buffer->link);
	pool_consider_destroy(buffer->pool);
	free(buffer);
}

static bool buffer_get_shm(struct nx_buffer *nx_buffer,
		struct nx_shm_attributes *attrs) {
	struct nx_shm_buffer *buffer = wl_container_of(nx_buffer, buffer, base);
	*attrs = (struct nx_shm_attributes){
		.fd = buffer->pool->fd,
		.format = buffer->drm_format,
		.width = buffer->base.width,
		.height = buffer->base.height,
		.stride = buffer->stride,
		.offset = buffer->offset,
	};
	return true;
}

static bool buffer_begin_data_ptr_access(struct nx_buffer *nx_buffer,
		uint32_t flags, void **data, uint32_t *format, size_t *stride) {
	struct nx_shm_buffer *buffer = wl_container_of(nx_buffer, buffer, base);
	struct nx_shm_mapping *mapping = buffer->pool->mapping;
	if (!nx_shm_mapping_begin_access(mapping, &buffer->sigbus_data)) {
		return false;
	}

	*data = (char *)mapping->data + buffer->offset;
	*format = buffer->drm_format;
	*stride = buffer->stride;
	return true;
}

static void buffer_end_data_ptr_access(struct nx_buffer *nx_buffer) {
	struct nx_shm_buffer *buffer = wl_container_of(nx_buffer, buffer, base);
	nx_shm_mapping_end_access(&buffer->sigbus_data);
}

static const struct nx_buffer_impl buffer_impl = {
	.destroy = buffer_destroy,
	.get_shm = buffer_get_shm,
	.begin_data_ptr_access = buffer_begin_data_ptr_access,
	.end_data_ptr_access = buffer_end_data_ptr_access,
};

static void destroy_resource(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct wl_buffer_interface wl_buffer_impl = {
	.destroy = destroy_resource,
};

static void buffer_handle_release(struct wl_listener *listener, void *data) {
	struct nx_shm_buffer *buffer = wl_container_of(listener, buffer, release);
	if (buffer->resource != NULL) {
		wl_buffer_send_release(buffer->resource);
	}
}

static void buffer_handle_resource_destroy(struct wl_resource *resource) {
	struct nx_shm_buffer *buffer = buffer_from_resource(resource);
	buffer->resource = NULL;
	nx_buffer_drop(&buffer->base);
}

static void pool_handle_create_buffer(struct wl_client *client,
		struct wl_resource *pool_resource, uint32_t id, int32_t offset,
		int32_t width, int32_t height, int32_t stride, uint32_t shm_format) {
	struct nx_shm_pool *pool = pool_from_resource(pool_resource);

	// 64-bit math avoids overflow
	if (offset < 0 || width <= 0 || height <= 0 || stride < width ||
			offset + (uint64_t)stride * height > pool->mapping->size) {
		wl_resource_post_error(pool_resource, WL_SHM_ERROR_INVALID_STRIDE,
			"Invalid width, height or stride (%dx%d, %d)",
			width, height, stride);
		return;
	}

	if (!shm_has_format(pool->shm, shm_format)) {
		wl_resource_post_error(pool_resource, WL_SHM_ERROR_INVALID_FORMAT,
			"Unsupported format");
		return;
	}

	uint32_t drm_format = convert_wl_shm_format_to_drm(shm_format);
	const struct nx_pixel_format_info *format_info =
		drm_get_pixel_format_info(drm_format);
	if (format_info == NULL) {
		wl_resource_post_error(pool_resource, WL_SHM_ERROR_INVALID_FORMAT,
			"Unknown format");
		return;
	}
	if (!pixel_format_info_check_stride(format_info, stride, width)) {
		wl_resource_post_error(pool_resource, WL_SHM_ERROR_INVALID_STRIDE,
			"Invalid stride (%d)", stride);
		return;
	}

	struct nx_shm_buffer *buffer = calloc(1, sizeof(*buffer));
	if (buffer == NULL) {
		wl_resource_post_no_memory(pool_resource);
		return;
	}

	buffer->resource = wl_resource_create(client, &wl_buffer_interface, 1, id);
	if (buffer->resource == NULL) {
		free(buffer);
		wl_resource_post_no_memory(pool_resource);
		return;
	}

	buffer->pool = pool;
	buffer->offset = offset;
	buffer->stride = stride;
	buffer->drm_format = drm_format;
	nx_buffer_init(&buffer->base, &buffer_impl, width, height);
	wl_resource_set_implementation(buffer->resource,
		&wl_buffer_impl, buffer, buffer_handle_resource_destroy);

	wl_list_insert(&pool->buffers, &buffer->link);

	buffer->release.notify = buffer_handle_release;
	wl_signal_add(&buffer->base.events.release, &buffer->release);
}

static void pool_handle_resize(struct wl_client *client,
		struct wl_resource *pool_resource, int32_t size) {
	struct nx_shm_pool *pool = pool_from_resource(pool_resource);

	if (size <= 0 || (size_t)size < pool->mapping->size) {
		wl_resource_post_error(pool_resource, WL_SHM_ERROR_INVALID_STRIDE,
			"Shrinking a pool (%zu to %d) is forbidden",
			pool->mapping->size, size);
		return;
	}

	struct nx_shm_mapping *mapping = nx_shm_mapping_create(pool->fd, size);
	if (mapping == NULL) {
		wl_resource_post_error(pool_resource, WL_SHM_ERROR_INVALID_FD,
			"Failed to create memory mapping");
		return;
	}

	nx_shm_mapping_drop(pool->mapping);
	pool->mapping = mapping;
}

static const struct wl_shm_pool_interface pool_impl = {
	.create_buffer = pool_handle_create_buffer,
	.destroy = destroy_resource,
	.resize = pool_handle_resize,
};

static void pool_handle_resource_destroy(struct wl_resource *resource) {
	struct nx_shm_pool *pool = pool_from_resource(resource);
	pool->resource = NULL;
	pool_consider_destroy(pool);
}

void nx_shm_create_pool(struct wl_client *client,
		struct wl_resource *shm_resource, uint32_t id, int fd, int32_t size) {
	struct nx_shm *shm = wl_resource_get_user_data(shm_resource);

	if (size <= 0) {
		wl_resource_post_error(shm_resource, WL_SHM_ERROR_INVALID_STRIDE,
			"Invalid size (%d)", size);
		goto error_fd;
	}

	struct nx_shm_mapping *mapping = nx_shm_mapping_create(fd, size);
	if (mapping == NULL) {
		wl_resource_post_error(shm_resource, WL_SHM_ERROR_INVALID_FD,
			"Failed to create memory mapping");
		goto error_fd;
	}

	struct nx_shm_pool *pool = calloc(1, sizeof(*pool));
	if (pool == NULL) {
		wl_resource_post_no_memory(shm_resource);
		goto error_mapping;
	}

	uint32_t version = wl_resource_get_version(shm_resource);
	pool->resource =
		wl_resource_create(client, &wl_shm_pool_interface, version, id);
	if (pool->resource == NULL) {
		wl_resource_post_no_memory(shm_resource);
		goto error_pool;
	}
	wl_resource_set_implementation(pool->resource, &pool_impl, pool,
		pool_handle_resource_destroy);

	pool->mapping = mapping;
	pool->shm = shm;
	pool->fd = fd;
	wl_list_init(&pool->buffers);
	return;

error_pool:
	free(pool);
error_mapping:
	nx_shm_mapping_drop(mapping);
error_fd:
	close(fd);
}
