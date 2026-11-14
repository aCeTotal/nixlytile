#ifndef PLATFORM_OUTPUT_COMMIT_COMMIT_H
#define PLATFORM_OUTPUT_COMMIT_COMMIT_H

#include "platform/render/drm_format_set.h"
#include "platform/output/output.h"

// Committed fields matching the current state.
uint32_t output_compare_state(struct nx_output *output,
	const struct nx_output_state *state);
bool output_basic_test(struct nx_output *output,
	const struct nx_output_state *state);
void output_apply_state(struct nx_output *output,
	const struct nx_output_state *state);

void output_pending_resolution(struct nx_output *output,
	const struct nx_output_state *state, int *width, int *height);
bool output_pending_enabled(struct nx_output *output,
	const struct nx_output_state *state);
const struct nx_output_image_description *output_pending_image_description(
	struct nx_output *output, const struct nx_output_state *state);

bool output_pick_format(struct nx_output *output,
	const struct nx_drm_format_set *display_formats,
	struct nx_drm_format *format, uint32_t fmt);
// May attach an empty modeset buffer.
bool output_ensure_buffer(struct nx_output *output,
	struct nx_output_state *state, bool *new_back_buffer);

void output_defer_present(struct nx_output *output, struct nx_output_event_present event);

bool output_prepare_commit(struct nx_output *output, const struct nx_output_state *state);
void output_apply_commit(struct nx_output *output, const struct nx_output_state *state);
void output_send_commit_event(struct nx_output *output, const struct nx_output_state *state);

void output_state_get_buffer_src_box(const struct nx_output_state *state,
	struct nx_fbox *out);
void output_state_get_buffer_dst_box(const struct nx_output_state *state,
	struct nx_box *out);

#endif
