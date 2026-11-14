#include <assert.h>
#include <stdlib.h>
#include "platform/output/output.h"
#include "platform/output/protocol/presentation_feedback.h"
#include "presentation-time-protocol.h"

struct nx_presentation_event {
	struct nx_output *output;
	uint64_t tv_sec;
	uint32_t tv_nsec;
	uint32_t refresh;
	uint64_t seq;
	uint32_t flags; // enum wp_presentation_feedback_kind
};

static void feedback_resource_send_presented(
		struct wl_resource *feedback_resource,
		const struct nx_presentation_event *event) {
	struct wl_client *client = wl_resource_get_client(feedback_resource);
	struct wl_resource *output_resource;
	wl_resource_for_each(output_resource, &event->output->resources) {
		if (wl_resource_get_client(output_resource) == client) {
			wp_presentation_feedback_send_sync_output(feedback_resource,
				output_resource);
		}
	}

	uint32_t tv_sec_hi = event->tv_sec >> 32;
	uint32_t tv_sec_lo = event->tv_sec & 0xFFFFFFFF;
	uint32_t seq_hi = event->seq >> 32;
	uint32_t seq_lo = event->seq & 0xFFFFFFFF;
	wp_presentation_feedback_send_presented(feedback_resource,
		tv_sec_hi, tv_sec_lo, event->tv_nsec, event->refresh,
		seq_hi, seq_lo, event->flags);

	wl_resource_destroy(feedback_resource);
}

static void feedback_send_presented(struct nx_presentation_feedback *feedback,
		const struct nx_presentation_event *event) {
	struct wl_resource *resource, *tmp;
	wl_resource_for_each_safe(resource, tmp, &feedback->resources) {
		feedback_resource_send_presented(resource, event);
	}
}

static void feedback_unset_output(struct nx_presentation_feedback *feedback) {
	if (feedback->output == NULL) {
		return;
	}

	feedback->output = NULL;
	wl_list_remove(&feedback->output_commit.link);
	wl_list_remove(&feedback->output_present.link);
	wl_list_remove(&feedback->output_destroy.link);
}

void output_presentation_feedback_destroy(struct nx_presentation_feedback *feedback) {
	if (feedback == NULL) {
		return;
	}

	struct wl_resource *resource, *tmp;
	wl_resource_for_each_safe(resource, tmp, &feedback->resources) {
		wp_presentation_feedback_send_discarded(resource);
		wl_resource_destroy(resource);
	}
	assert(wl_list_empty(&feedback->resources));

	feedback_unset_output(feedback);
	free(feedback);
}

static void feedback_handle_output_commit(struct wl_listener *listener,
		void *data) {
	struct nx_presentation_feedback *feedback =
		wl_container_of(listener, feedback, output_commit);
	if (feedback->output_committed) {
		return;
	}
	feedback->output_committed = true;
	feedback->output_commit_seq = feedback->output->commit_seq;
}

// Version 1 cannot express variable refresh.
static bool hide_refresh(struct nx_presentation_feedback *feedback,
		struct nx_output *output) {
	if (wl_list_empty(&feedback->resources)) {
		return false;
	}
	struct wl_resource *resource = wl_resource_from_link(feedback->resources.next);
	return wl_resource_get_version(resource) == 1 &&
		output->adaptive_sync_status == NX_OUTPUT_ADAPTIVE_SYNC_ENABLED;
}

static void feedback_send_output_present(struct nx_presentation_feedback *feedback,
		const struct nx_output_event_present *output_event) {
	struct nx_presentation_event event = {
		.output = output_event->output,
		.tv_sec = (uint64_t)output_event->when.tv_sec,
		.tv_nsec = (uint32_t)output_event->when.tv_nsec,
		.refresh = (uint32_t)output_event->refresh,
		.seq = (uint64_t)output_event->seq,
		.flags = output_event->flags,
	};
	if (hide_refresh(feedback, event.output)) {
		event.refresh = 0;
	}
	if (!feedback->zero_copy) {
		event.flags &= ~WP_PRESENTATION_FEEDBACK_KIND_ZERO_COPY;
	}
	feedback_send_presented(feedback, &event);
}

static void feedback_handle_output_present(struct wl_listener *listener,
		void *data) {
	struct nx_presentation_feedback *feedback =
		wl_container_of(listener, feedback, output_present);
	struct nx_output_event_present *output_event = data;

	if (!feedback->output_committed ||
			output_event->commit_seq != feedback->output_commit_seq) {
		return;
	}
	if (output_event->presented) {
		feedback_send_output_present(feedback, output_event);
	}
	output_presentation_feedback_destroy(feedback);
}

static void feedback_handle_output_destroy(struct wl_listener *listener,
		void *data) {
	struct nx_presentation_feedback *feedback =
		wl_container_of(listener, feedback, output_destroy);
	output_presentation_feedback_destroy(feedback);
}

static void presentation_surface_queued_on_output(struct nx_surface *surface,
		struct nx_output *output, bool zero_copy) {
	struct nx_presentation_feedback *feedback =
		output_presentation_surface_sampled(surface);
	if (feedback == NULL) {
		return;
	}

	assert(feedback->output == NULL);
	feedback->output = output;
	feedback->zero_copy = zero_copy;

	feedback->output_commit.notify = feedback_handle_output_commit;
	wl_signal_add(&output->events.commit, &feedback->output_commit);
	feedback->output_present.notify = feedback_handle_output_present;
	wl_signal_add(&output->events.present, &feedback->output_present);
	feedback->output_destroy.notify = feedback_handle_output_destroy;
	wl_signal_add(&output->events.destroy, &feedback->output_destroy);
}

void nx_presentation_surface_textured_on_output(struct nx_surface *surface,
		struct nx_output *output) {
	presentation_surface_queued_on_output(surface, output, false);
}

void nx_presentation_surface_scanned_out_on_output(struct nx_surface *surface,
		struct nx_output *output) {
	presentation_surface_queued_on_output(surface, output, true);
}
