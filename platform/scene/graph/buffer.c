#include <assert.h>
#include <stdlib.h>
#include "platform/buffer/buffer.h"
#include "platform/render/drm_syncobj.h"
#include "platform/render/texture.h"
#include "platform/scene/graph/buffer.h"
#include "platform/scene/graph/node.h"

static void scene_buffer_handle_buffer_release(struct wl_listener *listener,
		void *data) {
	struct nx_scene_buffer *scene_buffer =
		wl_container_of(listener, scene_buffer, buffer_release);

	scene_buffer->buffer = NULL;
	wl_list_remove(&scene_buffer->buffer_release.link);
	wl_list_init(&scene_buffer->buffer_release.link);
}

void scene_buffer_set_buffer(struct nx_scene_buffer *scene_buffer,
		struct nx_buffer *buffer) {
	wl_list_remove(&scene_buffer->buffer_release.link);
	wl_list_init(&scene_buffer->buffer_release.link);
	if (scene_buffer->own_buffer) {
		nx_buffer_unlock(scene_buffer->buffer);
	}
	scene_buffer->buffer = NULL;
	scene_buffer->own_buffer = false;
	scene_buffer->buffer_width = scene_buffer->buffer_height = 0;
	scene_buffer->buffer_is_opaque = false;

	if (!buffer) {
		return;
	}

	scene_buffer->own_buffer = true;
	scene_buffer->buffer = nx_buffer_lock(buffer);
	scene_buffer->buffer_width = buffer->width;
	scene_buffer->buffer_height = buffer->height;
	scene_buffer->buffer_is_opaque = nx_buffer_is_opaque(buffer);

	scene_buffer->buffer_release.notify = scene_buffer_handle_buffer_release;
	wl_signal_add(&buffer->events.release, &scene_buffer->buffer_release);
}

static void scene_buffer_handle_renderer_destroy(struct wl_listener *listener,
		void *data) {
	struct nx_scene_buffer *scene_buffer =
		wl_container_of(listener, scene_buffer, renderer_destroy);
	scene_buffer_set_texture(scene_buffer, NULL);
}

void scene_buffer_set_texture(struct nx_scene_buffer *scene_buffer,
		struct nx_texture *texture) {
	wl_list_remove(&scene_buffer->renderer_destroy.link);
	nx_texture_destroy(scene_buffer->texture);
	scene_buffer->texture = texture;

	if (texture == NULL) {
		wl_list_init(&scene_buffer->renderer_destroy.link);
		return;
	}

	scene_buffer->renderer_destroy.notify = scene_buffer_handle_renderer_destroy;
	wl_signal_add(&texture->renderer->events.destroy, &scene_buffer->renderer_destroy);
}

void scene_buffer_set_wait_timeline(struct nx_scene_buffer *scene_buffer,
		struct nx_drm_syncobj_timeline *timeline, uint64_t point) {
	nx_drm_syncobj_timeline_unref(scene_buffer->wait_timeline);
	scene_buffer->wait_timeline = NULL;
	scene_buffer->wait_point = 0;
	if (timeline == NULL) {
		return;
	}

	scene_buffer->wait_timeline = nx_drm_syncobj_timeline_ref(timeline);
	scene_buffer->wait_point = point;
}

struct nx_scene_buffer *nx_scene_buffer_create(struct nx_scene_tree *parent,
		struct nx_buffer *buffer) {
	struct nx_scene_buffer *scene_buffer = calloc(1, sizeof(*scene_buffer));
	if (scene_buffer == NULL) {
		return NULL;
	}
	assert(parent);
	scene_node_init(&scene_buffer->node, NX_SCENE_NODE_BUFFER, parent);

	wl_signal_init(&scene_buffer->events.outputs_update);
	wl_signal_init(&scene_buffer->events.output_sample);
	wl_signal_init(&scene_buffer->events.frame_done);

	pixman_region32_init(&scene_buffer->opaque_region);
	wl_list_init(&scene_buffer->buffer_release.link);
	wl_list_init(&scene_buffer->renderer_destroy.link);
	scene_buffer->opacity = 1;

	scene_buffer_set_buffer(scene_buffer, buffer);
	scene_node_update(&scene_buffer->node, NULL);

	return scene_buffer;
}

struct nx_texture *scene_buffer_get_texture(
		struct nx_scene_buffer *scene_buffer, struct nx_renderer *renderer) {
	if (scene_buffer->buffer == NULL || scene_buffer->texture != NULL) {
		return scene_buffer->texture;
	}

	struct nx_client_buffer *client_buffer =
		nx_client_buffer_get(scene_buffer->buffer);
	if (client_buffer != NULL) {
		return client_buffer->texture;
	}

	struct nx_texture *texture =
		nx_texture_from_buffer(renderer, scene_buffer->buffer);
	if (texture != NULL && scene_buffer->own_buffer) {
		scene_buffer->own_buffer = false;
		nx_buffer_unlock(scene_buffer->buffer);
	}
	scene_buffer_set_texture(scene_buffer, texture);
	return texture;
}
