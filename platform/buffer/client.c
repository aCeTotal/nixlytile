#include <assert.h>
#include <stdlib.h>
#include "platform/buffer/buffer_interface.h"
#include "platform/render/renderer.h"
#include "platform/log/log.h"
#include "platform/buffer/buffer.h"

static const struct nx_buffer_impl client_buffer_impl;

struct nx_client_buffer *nx_client_buffer_get(struct nx_buffer *nx_buffer) {
	if (nx_buffer->impl != &client_buffer_impl) {
		return NULL;
	}
	struct nx_client_buffer *buffer = wl_container_of(nx_buffer, buffer, base);
	return buffer;
}

static struct nx_client_buffer *client_buffer_from_buffer(
		struct nx_buffer *buffer) {
	struct nx_client_buffer *client_buffer = nx_client_buffer_get(buffer);
	assert(client_buffer != NULL);
	return client_buffer;
}

static void client_buffer_destroy(struct nx_buffer *buffer) {
	struct nx_client_buffer *client_buffer = client_buffer_from_buffer(buffer);

	nx_buffer_finish(buffer);

	wl_list_remove(&client_buffer->source_destroy.link);
	wl_list_remove(&client_buffer->renderer_destroy.link);
	nx_texture_destroy(client_buffer->texture);
	free(client_buffer);
}

static bool client_buffer_get_dmabuf(struct nx_buffer *buffer,
		struct nx_dmabuf_attributes *attribs) {
	struct nx_client_buffer *client_buffer = client_buffer_from_buffer(buffer);

	if (client_buffer->source == NULL) {
		return false;
	}

	return nx_buffer_get_dmabuf(client_buffer->source, attribs);
}

static bool client_buffer_get_shm(struct nx_buffer *buffer,
		struct nx_shm_attributes *attribs) {
	struct nx_client_buffer *client_buffer = client_buffer_from_buffer(buffer);

	if (client_buffer->source == NULL) {
		return false;
	}

	return nx_buffer_get_shm(client_buffer->source, attribs);
}

static bool client_buffer_begin_data_ptr_access(struct nx_buffer *buffer, uint32_t flags,
		void **data, uint32_t *format, size_t *stride) {
	struct nx_client_buffer *client_buffer = client_buffer_from_buffer(buffer);

	if (client_buffer->source == NULL) {
		return false;
	}

	return nx_buffer_begin_data_ptr_access(client_buffer->source, flags, data, format, stride);
}

static void client_buffer_end_data_ptr_access(struct nx_buffer *buffer) {
	struct nx_client_buffer *client_buffer = client_buffer_from_buffer(buffer);

	if (client_buffer->source == NULL) {
		return;
	}

	nx_buffer_end_data_ptr_access(client_buffer->source);
}

static const struct nx_buffer_impl client_buffer_impl = {
	.destroy = client_buffer_destroy,
	.get_dmabuf = client_buffer_get_dmabuf,
	.get_shm = client_buffer_get_shm,
	.begin_data_ptr_access = client_buffer_begin_data_ptr_access,
	.end_data_ptr_access = client_buffer_end_data_ptr_access,
};

static void client_buffer_handle_source_destroy(struct wl_listener *listener,
		void *data) {
	struct nx_client_buffer *client_buffer =
		wl_container_of(listener, client_buffer, source_destroy);
	wl_list_remove(&client_buffer->source_destroy.link);
	wl_list_init(&client_buffer->source_destroy.link);
	client_buffer->source = NULL;
}

static void client_buffer_handle_renderer_destroy(struct wl_listener *listener,
		void *data) {
	struct nx_client_buffer *client_buffer =
		wl_container_of(listener, client_buffer, renderer_destroy);
	wl_list_remove(&client_buffer->renderer_destroy.link);
	wl_list_init(&client_buffer->renderer_destroy.link);
	client_buffer->texture = NULL;
}

struct nx_client_buffer *nx_client_buffer_create(struct nx_buffer *buffer,
		struct nx_renderer *renderer) {
	struct nx_texture *texture = nx_texture_from_buffer(renderer, buffer);
	if (texture == NULL) {
		nx_log(NX_ERROR, "Failed to create texture");
		return NULL;
	}

	struct nx_client_buffer *client_buffer = calloc(1, sizeof(*client_buffer));
	if (client_buffer == NULL) {
		nx_texture_destroy(texture);
		return NULL;
	}
	nx_buffer_init(&client_buffer->base, &client_buffer_impl,
		texture->width, texture->height);
	client_buffer->source = buffer;
	client_buffer->texture = texture;

	wl_signal_add(&buffer->events.destroy, &client_buffer->source_destroy);
	client_buffer->source_destroy.notify = client_buffer_handle_source_destroy;

	wl_signal_add(&texture->renderer->events.destroy, &client_buffer->renderer_destroy);
	client_buffer->renderer_destroy.notify = client_buffer_handle_renderer_destroy;

	// Release fires before destroy
	nx_buffer_lock(&client_buffer->base);
	nx_buffer_drop(&client_buffer->base);

	return client_buffer;
}

bool nx_client_buffer_apply_damage(struct nx_client_buffer *client_buffer,
		struct nx_buffer *next, const pixman_region32_t *damage) {
	if (client_buffer->base.n_locks - client_buffer->n_ignore_locks > 1) {
		return false;
	}
	if (client_buffer->texture == NULL) {
		return false;
	}

	return nx_texture_update_from_buffer(client_buffer->texture, next, damage);
}
