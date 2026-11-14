#include <assert.h>
#include <stdlib.h>
#include "platform/log/log.h"
#include "platform/output/output_interface.h"
#include "platform/output/commit/commit.h"

struct deferred_present_event {
	struct nx_output *output;
	struct wl_event_source *idle_source;
	struct nx_output_event_present event;
	struct wl_listener output_destroy;
};

void nx_output_send_frame(struct nx_output *output) {
	output->frame_pending = false;
	if (output->enabled) {
		wl_signal_emit_mutable(&output->events.frame, output);
	}
}

static void schedule_frame_handle_idle_timer(void *data) {
	struct nx_output *output = data;
	output->idle_frame = NULL;
	if (!output->frame_pending) {
		nx_output_send_frame(output);
	}
}

void nx_output_schedule_frame(struct nx_output *output) {
	// Frame callbacks need a commit.
	nx_output_update_needs_frame(output);

	if (output->frame_pending || output->idle_frame != NULL) {
		return;
	}

	// A swap may follow immediately.
	output->idle_frame = wl_event_loop_add_idle(output->event_loop,
		schedule_frame_handle_idle_timer, output);
}

void nx_output_update_needs_frame(struct nx_output *output) {
	if (output->needs_frame) {
		return;
	}
	output->needs_frame = true;
	wl_signal_emit_mutable(&output->events.needs_frame, output);
}

void nx_output_send_present(struct nx_output *output,
		struct nx_output_event_present *event) {
	assert(event);
	event->output = output;

	bool unstamped = event->when.tv_sec == 0 && event->when.tv_nsec == 0;
	if (event->presented && unstamped &&
			clock_gettime(CLOCK_MONOTONIC, &event->when) != 0) {
		nx_log_errno(NX_ERROR, "failed to send output present event: "
			"failed to read clock");
		return;
	}

	wl_signal_emit_mutable(&output->events.present, event);
}

static void deferred_present_event_destroy(struct deferred_present_event *deferred) {
	wl_list_remove(&deferred->output_destroy.link);
	free(deferred);
}

static void deferred_present_event_handle_idle(void *data) {
	struct deferred_present_event *deferred = data;
	nx_output_send_present(deferred->output, &deferred->event);
	deferred_present_event_destroy(deferred);
}

static void deferred_present_event_handle_output_destroy(struct wl_listener *listener,
		void *data) {
	struct deferred_present_event *deferred =
		wl_container_of(listener, deferred, output_destroy);
	wl_event_source_remove(deferred->idle_source);
	deferred_present_event_destroy(deferred);
}

void output_defer_present(struct nx_output *output, struct nx_output_event_present event) {
	struct deferred_present_event *deferred = calloc(1, sizeof(*deferred));
	if (!deferred) {
		return;
	}
	*deferred = (struct deferred_present_event){
		.output = output,
		.event = event,
	};
	deferred->output_destroy.notify = deferred_present_event_handle_output_destroy;
	wl_signal_add(&output->events.destroy, &deferred->output_destroy);

	deferred->idle_source = wl_event_loop_add_idle(output->event_loop,
		deferred_present_event_handle_idle, deferred);
}
