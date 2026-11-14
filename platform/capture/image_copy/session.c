#include <assert.h>
#include <stdlib.h>
#include "platform/capture/image_copy/session.h"
#include "platform/render/pixel_format.h"
#include "ext-image-copy-capture-v1-protocol.h"

static const struct ext_image_copy_capture_session_v1_interface session_impl;

static struct image_copy_session *session_from_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource,
		&ext_image_copy_capture_session_v1_interface, &session_impl));
	return wl_resource_get_user_data(resource);
}

static void session_handle_destroy(struct wl_client *client,
		struct wl_resource *session_resource) {
	wl_resource_destroy(session_resource);
}

static void session_handle_create_frame(struct wl_client *client,
		struct wl_resource *session_resource, uint32_t new_id) {
	struct image_copy_session *session = session_from_resource(session_resource);

	if (session != NULL && session->frame != NULL) {
		wl_resource_post_error(session_resource,
			EXT_IMAGE_COPY_CAPTURE_SESSION_V1_ERROR_DUPLICATE_FRAME,
			"session already has a frame object");
		return;
	}

	image_copy_frame_create(session_resource, session, new_id);
}

static const struct ext_image_copy_capture_session_v1_interface session_impl = {
	.destroy = session_handle_destroy,
	.create_frame = session_handle_create_frame,
};

static void session_send_dmabuf_constraints(struct image_copy_session *session) {
	struct capture_source *source = session->source;

	if (source->dmabuf_formats.len > 0) {
		struct wl_array dev_id_array = {
			.data = &source->dmabuf_device,
			.size = sizeof(source->dmabuf_device),
		};
		ext_image_copy_capture_session_v1_send_dmabuf_device(session->resource,
			&dev_id_array);
	}
	for (size_t i = 0; i < source->dmabuf_formats.len; i++) {
		struct nx_drm_format *fmt = &source->dmabuf_formats.formats[i];
		struct wl_array modifiers_array = {
			.data = fmt->modifiers,
			.size = fmt->len * sizeof(fmt->modifiers[0]),
		};
		ext_image_copy_capture_session_v1_send_dmabuf_format(session->resource,
			fmt->format, &modifiers_array);
	}
}

static void session_send_constraints(struct image_copy_session *session) {
	struct capture_source *source = session->source;

	ext_image_copy_capture_session_v1_send_buffer_size(session->resource,
		source->width, source->height);

	for (size_t i = 0; i < source->shm_formats_len; i++) {
		ext_image_copy_capture_session_v1_send_shm_format(session->resource,
			convert_drm_format_to_wl_shm(source->shm_formats[i]));
	}

	session_send_dmabuf_constraints(session);

	ext_image_copy_capture_session_v1_send_done(session->resource);
}

static void session_destroy(struct image_copy_session *session) {
	if (session == NULL) {
		return;
	}

	if (session->frame != NULL) {
		image_copy_frame_fail(session->frame,
			EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_STOPPED);
	}

	if (session->source->impl->stop) {
		session->source->impl->stop(session->source);
	}

	ext_image_copy_capture_session_v1_send_stopped(session->resource);
	wl_resource_set_user_data(session->resource, NULL);

	pixman_region32_fini(&session->damage);
	wl_list_remove(&session->source_destroy.link);
	wl_list_remove(&session->source_constraints_update.link);
	wl_list_remove(&session->source_frame.link);

	free(session);
}

static void session_handle_source_destroy(struct wl_listener *listener,
		void *data) {
	struct image_copy_session *session =
		wl_container_of(listener, session, source_destroy);
	session_destroy(session);
}

static void session_handle_source_constraints_update(
		struct wl_listener *listener, void *data) {
	struct image_copy_session *session =
		wl_container_of(listener, session, source_constraints_update);
	session_send_constraints(session);
}

static void session_handle_source_frame(struct wl_listener *listener,
		void *data) {
	struct image_copy_session *session =
		wl_container_of(listener, session, source_frame);
	struct capture_source_frame_event *event = data;

	pixman_region32_union(&session->damage, &session->damage, event->damage);

	struct image_copy_frame *frame = session->frame;
	if (frame == NULL || !frame->capturing ||
			pixman_region32_empty(&session->damage)) {
		return;
	}

	pixman_region32_union(&frame->buffer_damage,
		&frame->buffer_damage, &session->damage);

	struct capture_source *source = frame->session->source;
	source->impl->copy_frame(source, frame, event);
}

static void session_handle_resource_destroy(struct wl_resource *resource) {
	session_destroy(session_from_resource(resource));
}

static void session_watch_source(struct image_copy_session *session,
		struct capture_source *source) {
	session->source_destroy.notify = session_handle_source_destroy;
	wl_signal_add(&source->events.destroy, &session->source_destroy);

	session->source_constraints_update.notify =
		session_handle_source_constraints_update;
	wl_signal_add(&source->events.constraints_update,
		&session->source_constraints_update);

	session->source_frame.notify = session_handle_source_frame;
	wl_signal_add(&source->events.frame, &session->source_frame);
}

void image_copy_session_create(struct wl_resource *parent_resource,
		uint32_t new_id, struct capture_source *source, uint32_t options) {
	struct wl_client *client = wl_resource_get_client(parent_resource);
	uint32_t version = wl_resource_get_version(parent_resource);
	struct wl_resource *session_resource = wl_resource_create(client,
		&ext_image_copy_capture_session_v1_interface, version, new_id);
	if (session_resource == NULL) {
		wl_resource_post_no_memory(parent_resource);
		return;
	}
	wl_resource_set_implementation(session_resource, &session_impl, NULL,
		session_handle_resource_destroy);

	if (source == NULL) {
		ext_image_copy_capture_session_v1_send_stopped(session_resource);
		return;
	}

	struct image_copy_session *session = calloc(1, sizeof(*session));
	if (session == NULL) {
		wl_resource_post_no_memory(parent_resource);
		return;
	}

	if (source->impl->start) {
		source->impl->start(source,
			options & EXT_IMAGE_COPY_CAPTURE_MANAGER_V1_OPTIONS_PAINT_CURSORS);
	}

	session->resource = session_resource;
	session->source = source;
	pixman_region32_init_rect(&session->damage, 0, 0, source->width,
		source->height);

	session_watch_source(session, source);

	wl_resource_set_user_data(session_resource, session);
	session_send_constraints(session);
}
