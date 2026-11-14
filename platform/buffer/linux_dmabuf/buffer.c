#include <assert.h>
#include <stdlib.h>
#include "linux-dmabuf-v1-protocol.h"
#include "platform/buffer/buffer_interface.h"
#include "platform/buffer/linux_dmabuf/requests.h"

struct nx_dmabuf_v1_buffer {
	struct nx_buffer base;

	struct wl_resource *resource; // NULL after client destroy
	struct nx_dmabuf_attributes attributes;
	struct wl_listener release;
};

static void buffer_handle_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct wl_buffer_interface wl_buffer_impl = {
	.destroy = buffer_handle_destroy,
};

static bool buffer_resource_is_instance(struct wl_resource *resource) {
	return wl_resource_instance_of(resource, &wl_buffer_interface,
		&wl_buffer_impl);
}

static struct nx_dmabuf_v1_buffer *dmabuf_v1_buffer_try_from_resource(
		struct wl_resource *resource) {
	if (!buffer_resource_is_instance(resource) ||
			wl_resource_get_user_data(resource) == NULL) {
		return NULL;
	}
	return wl_resource_get_user_data(resource);
}

static struct nx_buffer *buffer_from_resource(struct wl_resource *resource) {
	struct nx_dmabuf_v1_buffer *buffer =
		dmabuf_v1_buffer_try_from_resource(resource);
	assert(buffer != NULL);
	return &buffer->base;
}

const struct nx_buffer_resource_interface nx_dmabuf_v1_buffer_resource_interface = {
	.name = "nx_dmabuf_v1_buffer",
	.is_instance = buffer_resource_is_instance,
	.from_resource = buffer_from_resource,
};

static const struct nx_buffer_impl buffer_impl;

static struct nx_dmabuf_v1_buffer *dmabuf_v1_buffer_from_buffer(
		struct nx_buffer *nx_buffer) {
	assert(nx_buffer->impl == &buffer_impl);
	struct nx_dmabuf_v1_buffer *buffer = wl_container_of(nx_buffer, buffer, base);
	return buffer;
}

static void buffer_destroy(struct nx_buffer *nx_buffer) {
	struct nx_dmabuf_v1_buffer *buffer =
		dmabuf_v1_buffer_from_buffer(nx_buffer);
	wl_list_remove(&buffer->release.link);

	nx_buffer_finish(nx_buffer);

	if (buffer->resource != NULL) {
		wl_resource_set_user_data(buffer->resource, NULL);
	}
	nx_dmabuf_attributes_finish(&buffer->attributes);
	free(buffer);
}

static bool buffer_get_dmabuf(struct nx_buffer *nx_buffer,
		struct nx_dmabuf_attributes *attribs) {
	struct nx_dmabuf_v1_buffer *buffer =
		dmabuf_v1_buffer_from_buffer(nx_buffer);
	*attribs = buffer->attributes;
	return true;
}

static const struct nx_buffer_impl buffer_impl = {
	.destroy = buffer_destroy,
	.get_dmabuf = buffer_get_dmabuf,
};

static void buffer_handle_release(struct wl_listener *listener, void *data) {
	struct nx_dmabuf_v1_buffer *buffer =
		wl_container_of(listener, buffer, release);
	if (buffer->resource != NULL) {
		wl_buffer_send_release(buffer->resource);
	}
}

static void buffer_handle_resource_destroy(struct wl_resource *buffer_resource) {
	struct nx_dmabuf_v1_buffer *buffer =
		dmabuf_v1_buffer_try_from_resource(buffer_resource);
	assert(buffer != NULL);
	buffer->resource = NULL;
	nx_buffer_drop(&buffer->base);
}

bool nx_dmabuf_v1_buffer_create(struct wl_resource *params_resource,
		uint32_t buffer_id, const struct nx_dmabuf_attributes *attribs) {
	struct nx_dmabuf_v1_buffer *buffer = calloc(1, sizeof(*buffer));
	if (!buffer) {
		wl_resource_post_no_memory(params_resource);
		return false;
	}
	nx_buffer_init(&buffer->base, &buffer_impl, attribs->width, attribs->height);

	struct wl_client *client = wl_resource_get_client(params_resource);
	buffer->resource = wl_resource_create(client, &wl_buffer_interface,
		1, buffer_id);
	if (!buffer->resource) {
		wl_resource_post_no_memory(params_resource);
		free(buffer);
		return false;
	}
	wl_resource_set_implementation(buffer->resource,
		&wl_buffer_impl, buffer, buffer_handle_resource_destroy);

	buffer->attributes = *attribs;

	buffer->release.notify = buffer_handle_release;
	wl_signal_add(&buffer->base.events.release, &buffer->release);

	if (buffer_id == 0) {
		zwp_linux_buffer_params_v1_send_created(params_resource,
			buffer->resource);
	}
	return true;
}
