#include "platform/render/swapchain.h"
#include "platform/output/cursor/cursor.h"
#include "platform/output/commit/commit.h"
#include "platform/output/protocol/wl_output.h"

static void apply_image_description(struct nx_output *output,
		const struct nx_output_image_description *desc) {
	output->image_description = NULL;
	if (desc != NULL) {
		output->image_description_value = *desc;
		output->image_description = &output->image_description_value;
	}

	struct nx_output_cursor *cursor;
	wl_list_for_each(cursor, &output->cursors, link) {
		output_cursor_refresh_color_transform(cursor, output->image_description);
	}
}

static void apply_color_transform(struct nx_output *output,
		struct nx_color_transform *tr) {
	nx_color_transform_unref(output->color_transform);
	output->color_transform = tr != NULL ? nx_color_transform_ref(tr) : NULL;
}

// Returns whether the resolution or refresh changed.
static bool apply_mode(struct nx_output *output,
		const struct nx_output_state *state) {
	int width = 0, height = 0, refresh = 0;
	switch (state->mode_type) {
	case NX_OUTPUT_STATE_MODE_FIXED:
		output->current_mode = state->mode;
		if (state->mode != NULL) {
			width = state->mode->width;
			height = state->mode->height;
			refresh = state->mode->refresh;
		}
		break;
	case NX_OUTPUT_STATE_MODE_CUSTOM:
		output->current_mode = NULL;
		width = state->custom_mode.width;
		height = state->custom_mode.height;
		refresh = state->custom_mode.refresh;
		break;
	}

	if (output->width == width && output->height == height &&
			output->refresh == refresh) {
		return false;
	}
	output->width = width;
	output->height = height;
	output->refresh = refresh;

	struct nx_swapchain *swapchain = output->swapchain;
	if (swapchain != NULL &&
			(swapchain->width != width || swapchain->height != height)) {
		nx_swapchain_destroy(output->swapchain);
		output->swapchain = NULL;
	}
	return true;
}

void output_apply_state(struct nx_output *output,
		const struct nx_output_state *state) {
	uint32_t committed = state->committed;
	if (committed & NX_OUTPUT_STATE_RENDER_FORMAT) {
		output->render_format = state->render_format;
	}
	if (committed & NX_OUTPUT_STATE_ENABLED) {
		output->enabled = state->enabled;
	}
	if (committed & NX_OUTPUT_STATE_SCALE) {
		output->scale = state->scale;
	}
	if (committed & NX_OUTPUT_STATE_TRANSFORM) {
		output->transform = state->transform;
	}
	if (committed & NX_OUTPUT_STATE_IMAGE_DESCRIPTION) {
		apply_image_description(output, state->image_description);
	}
	if (committed & NX_OUTPUT_STATE_COLOR_TRANSFORM) {
		apply_color_transform(output, state->color_transform);
	}

	// Disabled outputs drop their swapchains.
	if ((committed & NX_OUTPUT_STATE_ENABLED) && !state->enabled) {
		nx_swapchain_destroy(output->swapchain);
		output->swapchain = NULL;
		nx_swapchain_destroy(output->cursor_swapchain);
		output->cursor_swapchain = NULL;
	}

	uint32_t changes = 0;
	if ((committed & NX_OUTPUT_STATE_MODE) && apply_mode(output, state)) {
		changes |= OUTPUT_GLOBAL_MODE;
	}
	if (committed & (NX_OUTPUT_STATE_MODE | NX_OUTPUT_STATE_TRANSFORM)) {
		changes |= OUTPUT_GLOBAL_GEOMETRY;
	}
	if (committed & NX_OUTPUT_STATE_SCALE) {
		changes |= OUTPUT_GLOBAL_SCALE;
	}
	if (changes != 0) {
		output_global_send_changes(output, changes);
	}
}
