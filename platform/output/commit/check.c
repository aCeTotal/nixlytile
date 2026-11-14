#include <assert.h>
#include "platform/backend/backend.h"
#include "platform/render/allocator/allocator.h"
#include "platform/log/log.h"
#include "platform/output/output_interface.h"
#include "platform/output/commit/commit.h"

static const struct {
	enum nx_output_state_field field;
	const char *name;
} needs_enabled[] = {
	{ NX_OUTPUT_STATE_BUFFER, "buffer" },
	{ NX_OUTPUT_STATE_MODE, "mode" },
	{ NX_OUTPUT_STATE_ADAPTIVE_SYNC_ENABLED, "adaptive sync" },
	{ NX_OUTPUT_STATE_RENDER_FORMAT, "render format" },
	{ NX_OUTPUT_STATE_COLOR_TRANSFORM, "color transform" },
	{ NX_OUTPUT_STATE_IMAGE_DESCRIPTION, "image description" },
};

static bool mode_unchanged(struct nx_output *output,
		const struct nx_output_state *state) {
	switch (state->mode_type) {
	case NX_OUTPUT_STATE_MODE_FIXED:
		return output->current_mode == state->mode;
	case NX_OUTPUT_STATE_MODE_CUSTOM:
		return output->width == state->custom_mode.width &&
			output->height == state->custom_mode.height &&
			output->refresh == state->custom_mode.refresh;
	}
	return false;
}

uint32_t output_compare_state(struct nx_output *output,
		const struct nx_output_state *state) {
	uint32_t committed = state->committed;
	bool adaptive_sync =
		output->adaptive_sync_status != NX_OUTPUT_ADAPTIVE_SYNC_DISABLED;

	uint32_t fields = 0;
	if ((committed & NX_OUTPUT_STATE_MODE) && mode_unchanged(output, state)) {
		fields |= NX_OUTPUT_STATE_MODE;
	}
	if ((committed & NX_OUTPUT_STATE_ENABLED) && output->enabled == state->enabled) {
		fields |= NX_OUTPUT_STATE_ENABLED;
	}
	if ((committed & NX_OUTPUT_STATE_SCALE) && output->scale == state->scale) {
		fields |= NX_OUTPUT_STATE_SCALE;
	}
	if ((committed & NX_OUTPUT_STATE_TRANSFORM) &&
			output->transform == state->transform) {
		fields |= NX_OUTPUT_STATE_TRANSFORM;
	}
	if ((committed & NX_OUTPUT_STATE_ADAPTIVE_SYNC_ENABLED) &&
			adaptive_sync == state->adaptive_sync_enabled) {
		fields |= NX_OUTPUT_STATE_ADAPTIVE_SYNC_ENABLED;
	}
	if ((committed & NX_OUTPUT_STATE_RENDER_FORMAT) &&
			output->render_format == state->render_format) {
		fields |= NX_OUTPUT_STATE_RENDER_FORMAT;
	}
	if ((committed & NX_OUTPUT_STATE_COLOR_TRANSFORM) &&
			output->color_transform == state->color_transform) {
		fields |= NX_OUTPUT_STATE_COLOR_TRANSFORM;
	}
	return fields;
}

static bool test_buffer(struct nx_output *output,
		const struct nx_output_state *state) {
	struct nx_fbox src_box;
	output_state_get_buffer_src_box(state, &src_box);
	if (src_box.x < 0.0 || src_box.y < 0.0 ||
			src_box.x + src_box.width > state->buffer->width ||
			src_box.y + src_box.height > state->buffer->height) {
		nx_log(NX_ERROR, "Tried to commit with invalid buffer_src_box");
		return false;
	}
	// Sub-pixel crops are allowed.
	if (nx_fbox_empty(&src_box)) {
		nx_log(NX_ERROR, "Tried to commit with an empty buffer_src_box");
		return false;
	}

	int width, height;
	output_pending_resolution(output, state, &width, &height);
	struct nx_box output_box = { .width = width, .height = height };
	struct nx_box dst_box;
	output_state_get_buffer_dst_box(state, &dst_box);
	// Partly off-screen is fine.
	if (!nx_box_intersects(&output_box, &dst_box)) {
		nx_log(NX_ERROR, "Primary buffer is entirely off-screen or 0-sized");
		return false;
	}
	return true;
}

static bool test_without_buffer(const struct nx_output_state *state) {
	if (state->tearing_page_flip) {
		nx_log(NX_ERROR, "Tried to commit a tearing page flip without a buffer");
		return false;
	}
	if (state->committed & NX_OUTPUT_STATE_WAIT_TIMELINE) {
		nx_log(NX_DEBUG, "Tried to set wait timeline without a buffer");
		return false;
	}
	if (state->committed & NX_OUTPUT_STATE_SIGNAL_TIMELINE) {
		nx_log(NX_DEBUG, "Tried to set signal timeline without a buffer");
		return false;
	}
	return true;
}

static bool test_render_format(struct nx_output *output, uint32_t render_format) {
	struct nx_allocator *allocator = output->allocator;
	assert(allocator != NULL);

	const struct nx_drm_format_set *display_formats =
		nx_output_get_primary_formats(output, allocator->buffer_caps);
	struct nx_drm_format format = {0};
	if (!output_pick_format(output, display_formats, &format, render_format)) {
		nx_log(NX_ERROR, "Failed to pick primary buffer format for output");
		return false;
	}
	nx_drm_format_finish(&format);
	return true;
}

static bool test_enabled(struct nx_output *output,
		const struct nx_output_state *state) {
	if (!(state->committed & (NX_OUTPUT_STATE_ENABLED | NX_OUTPUT_STATE_MODE))) {
		return true;
	}
	int width, height;
	output_pending_resolution(output, state, &width, &height);
	if (width == 0 || height == 0) {
		nx_log(NX_DEBUG, "Tried to enable an output with a zero mode");
		return false;
	}
	return true;
}

static bool test_disabled(const struct nx_output_state *state) {
	for (size_t i = 0; i < sizeof(needs_enabled) / sizeof(needs_enabled[0]); i++) {
		if (state->committed & needs_enabled[i].field) {
			nx_log(NX_DEBUG, "Tried to set %s on a disabled output",
				needs_enabled[i].name);
			return false;
		}
	}
	return true;
}

static bool test_image_description(struct nx_output *output,
		const struct nx_output_image_description *desc) {
	if (!(output->supported_primaries & desc->primaries)) {
		nx_log(NX_DEBUG, "Unsupported image description primaries");
		return false;
	}
	if (!(output->supported_transfer_functions & desc->transfer_function)) {
		nx_log(NX_DEBUG, "Unsupported image description transfer function");
		return false;
	}
	return true;
}

bool output_basic_test(struct nx_output *output,
		const struct nx_output_state *state) {
	uint32_t committed = state->committed;
	bool buffer_ok = (committed & NX_OUTPUT_STATE_BUFFER) ?
		test_buffer(output, state) : test_without_buffer(state);
	if (!buffer_ok) {
		return false;
	}
	if ((committed & NX_OUTPUT_STATE_RENDER_FORMAT) &&
			!test_render_format(output, state->render_format)) {
		return false;
	}

	bool enabled_ok = output_pending_enabled(output, state) ?
		test_enabled(output, state) : test_disabled(state);
	if (!enabled_ok) {
		return false;
	}

	if ((committed & (NX_OUTPUT_STATE_WAIT_TIMELINE | NX_OUTPUT_STATE_SIGNAL_TIMELINE)) &&
			!output->backend->features.timeline) {
		nx_log(NX_DEBUG, "Wait/signal timelines are not supported for this output");
		return false;
	}
	return !(committed & NX_OUTPUT_STATE_IMAGE_DESCRIPTION) ||
		state->image_description == NULL ||
		test_image_description(output, state->image_description);
}

bool nx_output_test_state(struct nx_output *output,
		const struct nx_output_state *state) {
	// Test only what would change.
	struct nx_output_state copy = *state;
	copy.committed &= ~output_compare_state(output, state);

	if (!output_basic_test(output, &copy)) {
		return false;
	}
	if (!output->impl->test) {
		return true;
	}

	bool new_back_buffer = false;
	if (!output_ensure_buffer(output, &copy, &new_back_buffer)) {
		return false;
	}

	bool ok = output->impl->test(output, &copy);
	if (new_back_buffer) {
		nx_buffer_unlock(copy.buffer);
	}
	return ok;
}
