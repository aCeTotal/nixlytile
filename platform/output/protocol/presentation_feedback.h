#ifndef PLATFORM_OUTPUT_PROTOCOL_PRESENTATION_FEEDBACK_H
#define PLATFORM_OUTPUT_PROTOCOL_PRESENTATION_FEEDBACK_H

#include "platform/surface/compositor.h"
#include "platform/output/presentation.h"

struct nx_presentation_feedback {
	struct wl_list resources; // wl_resource_get_link()

	// Set once queued.
	struct nx_output *output;
	bool output_committed;
	uint32_t output_commit_seq;
	bool zero_copy;

	struct wl_listener output_commit;
	struct wl_listener output_present;
	struct wl_listener output_destroy;
};

void output_presentation_feedback_destroy(struct nx_presentation_feedback *feedback);
// Takes the surface's current feedback.
struct nx_presentation_feedback *output_presentation_surface_sampled(
	struct nx_surface *surface);

#endif
