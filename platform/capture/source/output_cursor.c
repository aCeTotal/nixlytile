#include <assert.h>
#include <math.h>
#include "platform/capture/image_copy/session.h"
#include "platform/capture/source/output_source.h"
#include "platform/output/output.h"
#include "platform/output/output_interface.h"
#include "platform/render/swapchain.h"

static void output_cursor_request_frame(struct capture_source *base,
		bool schedule_frame) {
	struct output_cursor_source *cursor_source =
		wl_container_of(base, cursor_source, base);
	if (schedule_frame) {
		nx_output_update_needs_frame(cursor_source->output);
		cursor_source->needs_frame = true;
	}
}

static void output_cursor_copy_frame(struct capture_source *base,
		struct image_copy_frame *frame,
		struct capture_source_frame_event *base_event) {
	struct output_cursor_source *cursor_source =
		wl_container_of(base, cursor_source, base);

	struct nx_buffer *src_buffer = cursor_source->output->cursor_front_buffer;
	if (src_buffer == NULL) {
		image_copy_frame_fail(frame,
			EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_STOPPED);
		return;
	}

	if (!image_copy_frame_copy_buffer(frame, src_buffer,
			cursor_source->output->renderer)) {
		return;
	}

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);

	image_copy_frame_ready(frame, cursor_source->output->transform, &now);
}

static const struct capture_source_impl output_cursor_impl = {
	.request_frame = output_cursor_request_frame,
	.copy_frame = output_cursor_copy_frame,
};

static void output_cursor_update(struct output_cursor_source *cursor_source) {
	struct nx_output *output = cursor_source->output;
	struct capture_cursor_source *cursor = &cursor_source->base;

	if (output->cursor_swapchain != NULL && !cursor_source->initialized) {
		capture_source_set_constraints_from_swapchain(&cursor->base,
			output->cursor_swapchain, output->renderer);
		cursor_source->initialized = true;
	}

	struct nx_output_cursor *output_cursor = output->hardware_cursor;
	if (output_cursor == NULL || !output_cursor->visible) {
		cursor->entered = false;
		wl_signal_emit_mutable(&cursor->events.update, NULL);
		return;
	}

	if (output->cursor_swapchain != NULL &&
			((int)cursor->base.width != output->cursor_swapchain->width ||
			(int)cursor->base.height != output->cursor_swapchain->height)) {
		cursor->base.width = output->cursor_swapchain->width;
		cursor->base.height = output->cursor_swapchain->height;
		wl_signal_emit_mutable(&cursor->base.events.constraints_update, NULL);
	}

	cursor->entered = true;
	cursor->x = (int32_t)round(output_cursor->x);
	cursor->y = (int32_t)round(output_cursor->y);
	cursor->hotspot.x = output_cursor->hotspot_x;
	cursor->hotspot.y = output_cursor->hotspot_y;
	wl_signal_emit_mutable(&cursor->events.update, NULL);
}

static void output_cursor_handle_prev_buffer_release(
		struct wl_listener *listener, void *data) {
	struct output_cursor_source *cursor_source =
		wl_container_of(listener, cursor_source, prev_buffer_release);
	wl_list_remove(&cursor_source->prev_buffer_release.link);
	wl_list_init(&cursor_source->prev_buffer_release.link);
	cursor_source->prev_buffer = NULL;
}

static void output_cursor_send_frame(struct output_cursor_source *cursor_source,
		struct nx_buffer *buffer, const struct timespec *when) {
	pixman_region32_t full_damage;
	pixman_region32_init_rect(&full_damage, 0, 0, buffer->width, buffer->height);

	struct output_frame_event frame_event = {
		.base = {
			.damage = &full_damage,
		},
		.buffer = buffer,
		.when = *when,
	};
	wl_signal_emit_mutable(&cursor_source->base.base.events.frame, &frame_event);

	pixman_region32_fini(&full_damage);

	assert(buffer->n_locks > 0);
	cursor_source->prev_buffer = buffer;
	wl_list_remove(&cursor_source->prev_buffer_release.link);
	cursor_source->prev_buffer_release.notify =
		output_cursor_handle_prev_buffer_release;
	wl_signal_add(&buffer->events.release, &cursor_source->prev_buffer_release);
}

static void output_cursor_handle_output_commit(struct wl_listener *listener,
		void *data) {
	struct output_cursor_source *cursor_source =
		wl_container_of(listener, cursor_source, output_commit);
	struct nx_output_event_commit *event = data;

	output_cursor_update(cursor_source);

	struct nx_buffer *buffer = cursor_source->output->cursor_front_buffer;
	if (buffer != NULL && (buffer != cursor_source->prev_buffer ||
			cursor_source->needs_frame)) {
		output_cursor_send_frame(cursor_source, buffer, &event->when);
	}

	cursor_source->needs_frame = false;
}

void capture_output_cursor_init(struct output_cursor_source *cursor_source,
		struct nx_output *output) {
	capture_cursor_source_init(&cursor_source->base, &output_cursor_impl);

	cursor_source->output = output;

	cursor_source->output_commit.notify = output_cursor_handle_output_commit;
	wl_signal_add(&output->events.commit, &cursor_source->output_commit);

	wl_list_init(&cursor_source->prev_buffer_release.link);

	output_cursor_update(cursor_source);
}

void capture_output_cursor_finish(struct output_cursor_source *cursor_source) {
	capture_cursor_source_finish(&cursor_source->base);
	wl_list_remove(&cursor_source->output_commit.link);
	wl_list_remove(&cursor_source->prev_buffer_release.link);
}
