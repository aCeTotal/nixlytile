#include <assert.h>
#include <stdlib.h>
#include "platform/nested/output/output.h"
#include "platform/output/output_interface.h"
#include "platform/output/commit/commit.h"

#include "presentation-time-client-protocol.h"
#include "xdg-shell-client-protocol.h"

struct nx_wl_presentation_feedback {
	struct nx_wl_output *output;
	struct wl_list link; // nx_wl_output.presentation_feedbacks
	struct wp_presentation_feedback *feedback;
	uint32_t commit_seq;
};

static void surface_frame_callback(void *data, struct wl_callback *cb,
		uint32_t time) {
	struct nx_wl_output *output = data;
	assert(output->frame_callback == cb);
	wl_callback_destroy(cb);
	output->frame_callback = NULL;

	nx_output_send_frame(&output->nx_output);
}

static const struct wl_callback_listener frame_listener = {
	.done = surface_frame_callback,
};

static void presentation_feedback_destroy(
		struct nx_wl_presentation_feedback *feedback) {
	wl_list_remove(&feedback->link);
	wp_presentation_feedback_destroy(feedback->feedback);
	free(feedback);
}

static void presentation_feedback_handle_sync_output(void *data,
		struct wp_presentation_feedback *feedback, struct wl_output *output) {
}

static void presentation_feedback_handle_presented(void *data,
		struct wp_presentation_feedback *wp_feedback, uint32_t tv_sec_hi,
		uint32_t tv_sec_lo, uint32_t tv_nsec, uint32_t refresh_ns,
		uint32_t seq_hi, uint32_t seq_lo, uint32_t flags) {
	struct nx_wl_presentation_feedback *feedback = data;

	struct nx_output_event_present event = {
		.commit_seq = feedback->commit_seq,
		.presented = true,
		.when = {
			.tv_sec = ((uint64_t)tv_sec_hi << 32) | tv_sec_lo,
			.tv_nsec = tv_nsec,
		},
		.seq = ((uint64_t)seq_hi << 32) | seq_lo,
		.refresh = refresh_ns,
		.flags = flags,
	};
	nx_output_send_present(&feedback->output->nx_output, &event);

	presentation_feedback_destroy(feedback);
}

static void presentation_feedback_handle_discarded(void *data,
		struct wp_presentation_feedback *wp_feedback) {
	struct nx_wl_presentation_feedback *feedback = data;

	struct nx_output_event_present event = {
		.commit_seq = feedback->commit_seq,
		.presented = false,
	};
	nx_output_send_present(&feedback->output->nx_output, &event);

	presentation_feedback_destroy(feedback);
}

static const struct wp_presentation_feedback_listener
		presentation_feedback_listener = {
	.sync_output = presentation_feedback_handle_sync_output,
	.presented = presentation_feedback_handle_presented,
	.discarded = presentation_feedback_handle_discarded,
};

void nested_output_drop_feedbacks(struct nx_wl_output *output) {
	struct nx_wl_presentation_feedback *feedback, *feedback_tmp;
	wl_list_for_each_safe(feedback, feedback_tmp,
			&output->presentation_feedbacks, link) {
		presentation_feedback_destroy(feedback);
	}
}

static bool track_feedback(struct nx_wl_output *output,
		struct wp_presentation_feedback *wp_feedback) {
	struct nx_wl_presentation_feedback *feedback = calloc(1, sizeof(*feedback));
	if (feedback == NULL) {
		wp_presentation_feedback_destroy(wp_feedback);
		return false;
	}
	feedback->output = output;
	feedback->feedback = wp_feedback;
	feedback->commit_seq = output->nx_output.commit_seq + 1;
	wl_list_insert(&output->presentation_feedbacks, &feedback->link);

	wp_presentation_feedback_add_listener(wp_feedback,
		&presentation_feedback_listener, feedback);
	return true;
}

bool nested_output_submit(struct nx_wl_output *output) {
	struct nx_wl_backend *wl = output->backend;

	if (output->frame_callback != NULL) {
		wl_callback_destroy(output->frame_callback);
	}
	output->frame_callback = wl_surface_frame(output->surface);
	wl_callback_add_listener(output->frame_callback, &frame_listener, output);

	struct wp_presentation_feedback *wp_feedback = NULL;
	if (wl->presentation != NULL) {
		wp_feedback = wp_presentation_feedback(wl->presentation, output->surface);
	}

	if (output->has_configure_serial) {
		xdg_surface_ack_configure(output->xdg_surface, output->configure_serial);
		output->has_configure_serial = false;
	}
	wl_surface_commit(output->surface);

	if (wp_feedback != NULL) {
		return track_feedback(output, wp_feedback);
	}

	// Without feedback, assume immediate presentation.
	struct nx_output_event_present present_event = {
		.commit_seq = output->nx_output.commit_seq + 1,
		.presented = true,
	};
	output_defer_present(&output->nx_output, present_event);
	return true;
}
