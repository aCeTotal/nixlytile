#include <assert.h>
#include <drm_fourcc.h>
#include <inttypes.h>
#include <stdlib.h>
#include <xf86drm.h>
#include "platform/render/allocator/allocator.h"
#include "platform/render/drm_format_set.h"
#include "platform/render/swapchain.h"
#include "platform/log/log.h"
#include "platform/output/commit/commit.h"

static bool pick_primary_format(struct nx_output *output,
		uint32_t render_format, struct nx_drm_format *format) {
	struct nx_allocator *allocator = output->allocator;
	assert(output->allocator != NULL);

	const struct nx_drm_format_set *display_formats =
		nx_output_get_primary_formats(output, allocator->buffer_caps);
	if (!output_pick_format(output, display_formats, format, render_format)) {
		nx_log(NX_ERROR, "Failed to pick primary buffer format for output '%s'",
			output->name);
		return false;
	}

	char *format_name = drmGetFormatName(format->format);
	nx_log(NX_DEBUG, "Choosing primary buffer format %s (0x%08"PRIX32") for output '%s'",
		format_name ? format_name : "<unknown>", format->format, output->name);
	free(format_name);
	return true;
}

static bool restrict_to_implicit_modifier(struct nx_drm_format *format) {
	if (format->len == 1 && format->modifiers[0] == DRM_FORMAT_MOD_LINEAR) {
		return true;
	}
	if (!nx_drm_format_has(format, DRM_FORMAT_MOD_INVALID)) {
		nx_log(NX_DEBUG, "Implicit modifiers not supported");
		return false;
	}

	format->len = 0;
	if (!nx_drm_format_add(format, DRM_FORMAT_MOD_INVALID)) {
		nx_log(NX_DEBUG, "Failed to add implicit modifier to format");
		return false;
	}
	return true;
}

static bool test_swapchain(struct nx_output *output,
		struct nx_swapchain *swapchain, const struct nx_output_state *state) {
	struct nx_buffer *buffer = nx_swapchain_acquire(swapchain);
	if (buffer == NULL) {
		return false;
	}

	struct nx_output_state copy = *state;
	copy.committed |= NX_OUTPUT_STATE_BUFFER;
	copy.buffer = buffer;
	bool ok = nx_output_test_state(output, &copy);
	nx_buffer_unlock(buffer);
	return ok;
}

static struct nx_swapchain *create_implicit_swapchain(struct nx_output *output,
		const struct nx_output_state *state, struct nx_drm_format *format) {
	int width, height;
	output_pending_resolution(output, state, &width, &height);

	struct nx_swapchain *swapchain = NULL;
	if (restrict_to_implicit_modifier(format)) {
		swapchain = nx_swapchain_create(output->allocator, width, height, format);
	}
	if (swapchain == NULL) {
		nx_log(NX_ERROR, "Failed to create modifier-less swapchain for output '%s'",
			output->name);
		return NULL;
	}

	nx_log(NX_DEBUG, "Testing modifier-less swapchain for output '%s'", output->name);
	if (!test_swapchain(output, swapchain, state)) {
		nx_log(NX_ERROR, "Swapchain for output '%s' failed test", output->name);
		nx_swapchain_destroy(swapchain);
		return NULL;
	}
	return swapchain;
}

// Falls back to implicit modifiers.
static struct nx_swapchain *create_tested_swapchain(struct nx_output *output,
		const struct nx_output_state *state, struct nx_drm_format *format) {
	int width, height;
	output_pending_resolution(output, state, &width, &height);

	struct nx_swapchain *swapchain =
		nx_swapchain_create(output->allocator, width, height, format);
	if (swapchain == NULL) {
		nx_log(NX_ERROR, "Failed to create swapchain for output '%s'", output->name);
		return NULL;
	}

	nx_log(NX_DEBUG, "Testing swapchain for output '%s'", output->name);
	if (test_swapchain(output, swapchain, state)) {
		return swapchain;
	}

	nx_log(NX_DEBUG, "Output test failed on '%s', retrying without modifiers",
		output->name);
	nx_swapchain_destroy(swapchain);
	return create_implicit_swapchain(output, state, format);
}

bool nx_output_configure_primary_swapchain(struct nx_output *output,
		const struct nx_output_state *state, struct nx_swapchain **swapchain_ptr) {
	struct nx_output_state empty_state;
	if (state == NULL) {
		nx_output_state_init(&empty_state);
		state = &empty_state;
	}

	int width, height;
	output_pending_resolution(output, state, &width, &height);
	uint32_t render_format = (state->committed & NX_OUTPUT_STATE_RENDER_FORMAT) ?
		state->render_format : output->render_format;

	struct nx_swapchain *old = *swapchain_ptr;
	if (old != NULL && old->width == width && old->height == height &&
			old->format.format == render_format) {
		return true;
	}

	struct nx_drm_format format = {0};
	if (!pick_primary_format(output, render_format, &format)) {
		nx_log(NX_ERROR, "Failed to create swapchain for output '%s'", output->name);
		return false;
	}
	struct nx_swapchain *swapchain = create_tested_swapchain(output, state, &format);
	nx_drm_format_finish(&format);
	if (swapchain == NULL) {
		return false;
	}

	nx_swapchain_destroy(*swapchain_ptr);
	*swapchain_ptr = swapchain;
	return true;
}
