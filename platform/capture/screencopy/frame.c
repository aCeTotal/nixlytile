#include <assert.h>
#include <stdlib.h>
#include "platform/capture/screencopy/frame.h"
#include "platform/output/output_interface.h"
#include "platform/render/dmabuf.h"
#include "wlr-screencopy-unstable-v1-protocol.h"

static const struct zwlr_screencopy_frame_v1_interface frame_impl;

static struct screencopy_frame *frame_from_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource,
		&zwlr_screencopy_frame_v1_interface, &frame_impl));
	return wl_resource_get_user_data(resource);
}

static void frame_unlock_output(struct screencopy_frame *frame) {
	if (frame->output == NULL || frame->buffer == NULL) {
		return;
	}
	nx_output_lock_attach_render(frame->output, false);
	if (frame->cursor_locked) {
		nx_output_lock_software_cursors(frame->output, false);
	}
}

static void frame_destroy(struct screencopy_frame *frame) {
	if (frame == NULL) {
		return;
	}
	frame_unlock_output(frame);
	wl_list_remove(&frame->output_commit.link);
	wl_list_remove(&frame->output_destroy.link);
	wl_resource_set_user_data(frame->resource, NULL);
	nx_buffer_unlock(frame->buffer);
	screencopy_client_unref(frame->client);
	free(frame);
}

void screencopy_frame_fail(struct screencopy_frame *frame) {
	zwlr_screencopy_frame_v1_send_failed(frame->resource);
	frame_destroy(frame);
}

static void frame_send_damage(struct screencopy_frame *frame) {
	if (!frame->with_damage) {
		return;
	}

	pixman_region32_t *damage =
		screencopy_client_damage(frame->client, frame->output);
	if (damage == NULL) {
		return;
	}

	int n_boxes;
	const pixman_box32_t *boxes = pixman_region32_rectangles(damage, &n_boxes);
	for (int i = 0; i < n_boxes; i++) {
		const pixman_box32_t *box = &boxes[i];
		zwlr_screencopy_frame_v1_send_damage(frame->resource,
			box->x1, box->y1, box->x2 - box->x1, box->y2 - box->y1);
	}

	pixman_region32_clear(damage);
}

static void frame_send_ready(struct screencopy_frame *frame,
		struct timespec *when) {
	time_t tv_sec = when->tv_sec;
	uint32_t tv_sec_hi = (sizeof(tv_sec) > 4) ? tv_sec >> 32 : 0;
	uint32_t tv_sec_lo = tv_sec & 0xFFFFFFFF;
	zwlr_screencopy_frame_v1_send_ready(frame->resource,
		tv_sec_hi, tv_sec_lo, when->tv_nsec);
}

// True until new damage arrives.
static bool frame_awaits_damage(struct screencopy_frame *frame) {
	if (!frame->with_damage) {
		return false;
	}
	pixman_region32_t *damage =
		screencopy_client_damage(frame->client, frame->output);
	return damage != NULL && pixman_region32_empty(damage);
}

static bool frame_copy(struct screencopy_frame *frame,
		struct nx_buffer *src_buffer) {
	if (frame->box.x < 0 || frame->box.y < 0 ||
			frame->box.x + frame->box.width > src_buffer->width ||
			frame->box.y + frame->box.height > src_buffer->height) {
		return false;
	}

	switch (frame->buffer_cap) {
	case NX_BUFFER_CAP_DMABUF:
		return screencopy_copy_dmabuf(frame, src_buffer);
	case NX_BUFFER_CAP_DATA_PTR:
		return screencopy_copy_shm(frame, src_buffer);
	default:
		abort();
	}
}

static void frame_handle_output_commit(struct wl_listener *listener,
		void *data) {
	struct screencopy_frame *frame =
		wl_container_of(listener, frame, output_commit);
	struct nx_output_event_commit *event = data;

	if ((event->state->committed & NX_OUTPUT_STATE_ENABLED) &&
			!frame->output->enabled) {
		screencopy_frame_fail(frame);
		return;
	}
	if (!(event->state->committed & NX_OUTPUT_STATE_BUFFER) ||
			frame->buffer == NULL || frame_awaits_damage(frame)) {
		return;
	}

	wl_list_remove(&frame->output_commit.link);
	wl_list_init(&frame->output_commit.link);

	if (!frame_copy(frame, event->state->buffer)) {
		screencopy_frame_fail(frame);
		return;
	}

	zwlr_screencopy_frame_v1_send_flags(frame->resource, 0);
	frame_send_damage(frame);
	frame_send_ready(frame, &event->when);
	frame_destroy(frame);
}

static void frame_handle_output_destroy(struct wl_listener *listener,
		void *data) {
	struct screencopy_frame *frame =
		wl_container_of(listener, frame, output_destroy);
	screencopy_frame_fail(frame);
}

static void frame_post_invalid_buffer(struct screencopy_frame *frame,
		const char *message) {
	wl_resource_post_error(frame->resource,
		ZWLR_SCREENCOPY_FRAME_V1_ERROR_INVALID_BUFFER, "%s", message);
}

static bool frame_accept_dmabuf(struct screencopy_frame *frame,
		uint32_t format) {
	if (format != frame->dmabuf_format) {
		frame_post_invalid_buffer(frame, "invalid buffer format");
		return false;
	}
	frame->buffer_cap = NX_BUFFER_CAP_DMABUF;
	return true;
}

// Posts an error when false.
static bool frame_set_buffer_cap(struct screencopy_frame *frame,
		struct nx_buffer *buffer) {
	struct nx_dmabuf_attributes dmabuf;
	if (nx_buffer_get_dmabuf(buffer, &dmabuf)) {
		return frame_accept_dmabuf(frame, dmabuf.format);
	}

	void *data;
	uint32_t format;
	size_t stride;
	if (!nx_buffer_begin_data_ptr_access(buffer,
			NX_BUFFER_DATA_PTR_ACCESS_WRITE, &data, &format, &stride)) {
		frame_post_invalid_buffer(frame, "unsupported buffer type");
		return false;
	}
	nx_buffer_end_data_ptr_access(buffer);

	if (format != frame->shm_format) {
		frame_post_invalid_buffer(frame, "invalid buffer format");
		return false;
	}
	if (stride != (size_t)frame->shm_stride) {
		frame_post_invalid_buffer(frame, "invalid buffer stride");
		return false;
	}
	frame->buffer_cap = NX_BUFFER_CAP_DATA_PTR;
	return true;
}

static void frame_arm(struct screencopy_frame *frame) {
	struct nx_output *output = frame->output;

	wl_signal_add(&output->events.commit, &frame->output_commit);
	frame->output_commit.notify = frame_handle_output_commit;

	// Front buffer may be released.
	nx_output_update_needs_frame(output);

	nx_output_lock_attach_render(output, true);
	if (frame->overlay_cursor) {
		nx_output_lock_software_cursors(output, true);
		frame->cursor_locked = true;
	}
}

static void frame_handle_copy(struct wl_client *wl_client,
		struct wl_resource *frame_resource,
		struct wl_resource *buffer_resource) {
	struct screencopy_frame *frame = frame_from_resource(frame_resource);
	if (frame == NULL) {
		return;
	}

	if (!frame->output->enabled) {
		screencopy_frame_fail(frame);
		return;
	}

	struct nx_buffer *buffer = nx_buffer_try_from_resource(buffer_resource);
	if (buffer == NULL) {
		frame_post_invalid_buffer(frame, "invalid buffer");
		return;
	}

	if (buffer->width != frame->box.width || buffer->height != frame->box.height) {
		frame_post_invalid_buffer(frame, "invalid buffer dimensions");
		return;
	}

	if (frame->buffer != NULL) {
		wl_resource_post_error(frame->resource,
			ZWLR_SCREENCOPY_FRAME_V1_ERROR_ALREADY_USED,
			"frame already used");
		return;
	}

	if (!frame_set_buffer_cap(frame, buffer)) {
		return;
	}

	frame->buffer = buffer;
	frame_arm(frame);
}

static void frame_handle_copy_with_damage(struct wl_client *wl_client,
		struct wl_resource *frame_resource,
		struct wl_resource *buffer_resource) {
	struct screencopy_frame *frame = frame_from_resource(frame_resource);
	if (frame == NULL) {
		return;
	}
	frame->with_damage = true;
	frame_handle_copy(wl_client, frame_resource, buffer_resource);
}

static void frame_handle_destroy(struct wl_client *wl_client,
		struct wl_resource *frame_resource) {
	wl_resource_destroy(frame_resource);
}

static const struct zwlr_screencopy_frame_v1_interface frame_impl = {
	.copy = frame_handle_copy,
	.destroy = frame_handle_destroy,
	.copy_with_damage = frame_handle_copy_with_damage,
};

static void frame_handle_resource_destroy(struct wl_resource *frame_resource) {
	frame_destroy(frame_from_resource(frame_resource));
}

struct screencopy_frame *screencopy_frame_create(
		struct wl_resource *manager_resource, uint32_t id) {
	struct wl_client *wl_client = wl_resource_get_client(manager_resource);
	struct screencopy_frame *frame = calloc(1, sizeof(*frame));
	if (frame == NULL) {
		wl_client_post_no_memory(wl_client);
		return NULL;
	}

	frame->resource = wl_resource_create(wl_client,
		&zwlr_screencopy_frame_v1_interface,
		wl_resource_get_version(manager_resource), id);
	if (frame->resource == NULL) {
		free(frame);
		wl_client_post_no_memory(wl_client);
		return NULL;
	}
	wl_resource_set_implementation(frame->resource, &frame_impl, frame,
		frame_handle_resource_destroy);
	return frame;
}

void screencopy_frame_start(struct screencopy_frame *frame,
		struct screencopy_client *client) {
	frame->client = client;
	client->ref++;

	wl_list_init(&frame->output_commit.link);

	wl_signal_add(&frame->output->events.destroy, &frame->output_destroy);
	frame->output_destroy.notify = frame_handle_output_destroy;
}
