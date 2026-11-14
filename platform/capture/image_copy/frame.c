#include <assert.h>
#include <stdlib.h>
#include "platform/buffer/buffer.h"
#include "platform/capture/image_copy/session.h"
#include "ext-image-copy-capture-v1-protocol.h"

static const struct ext_image_copy_capture_frame_v1_interface frame_impl;

static struct image_copy_frame *frame_from_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource,
		&ext_image_copy_capture_frame_v1_interface, &frame_impl));
	return wl_resource_get_user_data(resource);
}

static void frame_destroy(struct image_copy_frame *frame) {
	if (frame == NULL) {
		return;
	}
	wl_resource_set_user_data(frame->resource, NULL);
	nx_buffer_unlock(frame->buffer);
	pixman_region32_fini(&frame->buffer_damage);
	if (frame->session->frame == frame) {
		frame->session->frame = NULL;
	}
	free(frame);
}

static void frame_handle_resource_destroy(struct wl_resource *resource) {
	frame_destroy(wl_resource_get_user_data(resource));
}

void image_copy_frame_ready(struct image_copy_frame *frame,
		enum wl_output_transform transform,
		const struct timespec *presentation_time) {
	assert(frame->capturing);

	int rects_len = 0;
	const pixman_box32_t *rects =
		pixman_region32_rectangles(&frame->session->damage, &rects_len);
	for (int i = 0; i < rects_len; i++) {
		const pixman_box32_t *rect = &rects[i];
		ext_image_copy_capture_frame_v1_send_damage(frame->resource,
			rect->x1, rect->y1, rect->x2 - rect->x1, rect->y2 - rect->y1);
	}

	pixman_region32_clear(&frame->session->damage);

	uint64_t pres_time_sec = (uint64_t)presentation_time->tv_sec;
	ext_image_copy_capture_frame_v1_send_transform(frame->resource, transform);
	ext_image_copy_capture_frame_v1_send_presentation_time(frame->resource,
		pres_time_sec >> 32, (uint32_t)pres_time_sec, presentation_time->tv_nsec);
	ext_image_copy_capture_frame_v1_send_ready(frame->resource);
	frame_destroy(frame);
}

void image_copy_frame_fail(struct image_copy_frame *frame,
		enum ext_image_copy_capture_frame_v1_failure_reason reason) {
	ext_image_copy_capture_frame_v1_send_failed(frame->resource, reason);
	frame_destroy(frame);
}

static void frame_handle_destroy(struct wl_client *client,
		struct wl_resource *frame_resource) {
	wl_resource_destroy(frame_resource);
}

// False after posting already-captured.
static bool frame_check_not_capturing(struct image_copy_frame *frame,
		const char *message) {
	if (!frame->capturing) {
		return true;
	}
	wl_resource_post_error(frame->resource,
		EXT_IMAGE_COPY_CAPTURE_FRAME_V1_ERROR_ALREADY_CAPTURED,
		"%s", message);
	return false;
}

static void frame_handle_attach_buffer(struct wl_client *client,
		struct wl_resource *frame_resource, struct wl_resource *buffer_resource) {
	struct image_copy_frame *frame = frame_from_resource(frame_resource);
	if (frame == NULL ||
			!frame_check_not_capturing(frame, "attach_buffer sent after capture")) {
		return;
	}

	struct nx_buffer *buffer = nx_buffer_try_from_resource(buffer_resource);
	if (buffer == NULL) {
		wl_resource_post_no_memory(frame_resource);
		return;
	}

	nx_buffer_unlock(frame->buffer);
	frame->buffer = buffer;
}

static void frame_handle_damage_buffer(struct wl_client *client,
		struct wl_resource *frame_resource, int32_t x, int32_t y,
		int32_t width, int32_t height) {
	struct image_copy_frame *frame = frame_from_resource(frame_resource);
	if (frame == NULL ||
			!frame_check_not_capturing(frame, "damage_buffer sent after capture")) {
		return;
	}

	if (x < 0 || y < 0 || width <= 0 || height <= 0) {
		wl_resource_post_error(frame->resource,
			EXT_IMAGE_COPY_CAPTURE_FRAME_V1_ERROR_INVALID_BUFFER_DAMAGE,
			"Invalid buffer damage coordinates");
		return;
	}

	pixman_region32_union_rect(&frame->buffer_damage, &frame->buffer_damage,
		x, y, width, height);
}

static void frame_handle_capture(struct wl_client *client,
		struct wl_resource *frame_resource) {
	struct image_copy_frame *frame = frame_from_resource(frame_resource);
	if (frame == NULL ||
			!frame_check_not_capturing(frame, "capture sent twice")) {
		return;
	}

	if (frame->buffer == NULL) {
		wl_resource_post_error(frame->resource,
			EXT_IMAGE_COPY_CAPTURE_FRAME_V1_ERROR_NO_BUFFER,
			"No buffer attached");
		return;
	}

	frame->capturing = true;

	bool schedule_frame = !pixman_region32_empty(&frame->session->damage);
	struct capture_source *source = frame->session->source;
	if (source->impl->request_frame) {
		source->impl->request_frame(source, schedule_frame);
	}
}

static const struct ext_image_copy_capture_frame_v1_interface frame_impl = {
	.destroy = frame_handle_destroy,
	.attach_buffer = frame_handle_attach_buffer,
	.damage_buffer = frame_handle_damage_buffer,
	.capture = frame_handle_capture,
};

void image_copy_frame_create(struct wl_resource *session_resource,
		struct image_copy_session *session, uint32_t new_id) {
	struct wl_resource *frame_resource = wl_resource_create(
		wl_resource_get_client(session_resource),
		&ext_image_copy_capture_frame_v1_interface,
		wl_resource_get_version(session_resource), new_id);
	if (frame_resource == NULL) {
		wl_resource_post_no_memory(frame_resource);
		return;
	}
	wl_resource_set_implementation(frame_resource, &frame_impl, NULL,
		frame_handle_resource_destroy);

	if (session == NULL) {
		ext_image_copy_capture_frame_v1_send_failed(frame_resource,
			EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_STOPPED);
		return;
	}

	struct image_copy_frame *frame = calloc(1, sizeof(*frame));
	if (frame == NULL) {
		wl_resource_post_no_memory(session_resource);
		return;
	}

	frame->resource = frame_resource;
	frame->session = session;
	pixman_region32_init(&frame->buffer_damage);

	wl_resource_set_user_data(frame_resource, frame);

	session->frame = frame;
}
