#include <stdlib.h>
#include "platform/output/commit/commit.h"

void output_pending_resolution(struct nx_output *output,
		const struct nx_output_state *state, int *width, int *height) {
	if (!(state->committed & NX_OUTPUT_STATE_MODE)) {
		*width = output->width;
		*height = output->height;
		return;
	}
	switch (state->mode_type) {
	case NX_OUTPUT_STATE_MODE_FIXED:
		*width = state->mode->width;
		*height = state->mode->height;
		return;
	case NX_OUTPUT_STATE_MODE_CUSTOM:
		*width = state->custom_mode.width;
		*height = state->custom_mode.height;
		return;
	}
	abort();
}

bool output_pending_enabled(struct nx_output *output,
		const struct nx_output_state *state) {
	if (state->committed & NX_OUTPUT_STATE_ENABLED) {
		return state->enabled;
	}
	return output->enabled;
}

const struct nx_output_image_description *output_pending_image_description(
		struct nx_output *output, const struct nx_output_state *state) {
	if (state->committed & NX_OUTPUT_STATE_IMAGE_DESCRIPTION) {
		return state->image_description;
	}
	return output->image_description;
}

void output_state_get_buffer_src_box(const struct nx_output_state *state,
		struct nx_fbox *out) {
	*out = state->buffer_src_box;
	// Unset means the whole buffer.
	if (state->buffer_src_box.width == 0.0 &&
			state->buffer_src_box.height == 0.0) {
		out->width = (double)state->buffer->width;
		out->height = (double)state->buffer->height;
	}
}

void output_state_get_buffer_dst_box(const struct nx_output_state *state,
		struct nx_box *out) {
	*out = state->buffer_dst_box;
	// Unset means the source size.
	if (state->buffer_dst_box.width == 0 &&
			state->buffer_dst_box.height == 0) {
		struct nx_fbox src_box;
		output_state_get_buffer_src_box(state, &src_box);
		out->width = (int)src_box.width;
		out->height = (int)src_box.height;
	}
}
