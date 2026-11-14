#include <assert.h>
#include <inttypes.h>
#include "platform/render/drm_format_set.h"
#include "platform/render/renderer.h"
#include "platform/log/log.h"
#include "platform/output/output_interface.h"
#include "platform/output/commit/commit.h"

const struct nx_drm_format_set *nx_output_get_primary_formats(
		struct nx_output *output, uint32_t buffer_caps) {
	if (!output->impl->get_primary_formats) {
		return NULL;
	}

	const struct nx_drm_format_set *formats =
		output->impl->get_primary_formats(output, buffer_caps);
	if (formats == NULL) {
		nx_log(NX_ERROR, "Failed to get primary display formats");
		static const struct nx_drm_format_set empty_format_set = {0};
		return &empty_format_set;
	}
	return formats;
}

static bool format_has_modifiers(struct nx_drm_format *format) {
	if (format->len != 0) {
		return true;
	}
	nx_drm_format_finish(format);
	nx_log(NX_DEBUG, "Failed to pick output format");
	return false;
}

bool output_pick_format(struct nx_output *output,
		const struct nx_drm_format_set *display_formats,
		struct nx_drm_format *format, uint32_t fmt) {
	struct nx_renderer *renderer = output->renderer;
	struct nx_allocator *allocator = output->allocator;
	assert(renderer != NULL && allocator != NULL);

	const struct nx_drm_format_set *render_formats =
		nx_renderer_get_render_formats(renderer);
	if (render_formats == NULL) {
		nx_log(NX_ERROR, "Failed to get render formats");
		return false;
	}

	const struct nx_drm_format *render_format =
		nx_drm_format_set_get(render_formats, fmt);
	if (render_format == NULL) {
		nx_log(NX_DEBUG, "Renderer doesn't support format 0x%"PRIX32, fmt);
		return false;
	}

	// The output displays any format.
	if (display_formats == NULL) {
		return nx_drm_format_copy(format, render_format) &&
			format_has_modifiers(format);
	}

	const struct nx_drm_format *display_format =
		nx_drm_format_set_get(display_formats, fmt);
	if (display_format == NULL) {
		nx_log(NX_DEBUG, "Output doesn't support format 0x%"PRIX32, fmt);
		return false;
	}
	if (!nx_drm_format_intersect(format, display_format, render_format)) {
		nx_log(NX_DEBUG, "Failed to intersect display and render "
			"modifiers for format 0x%"PRIX32 " on output %s",
			fmt, output->name);
		return false;
	}
	return format_has_modifiers(format);
}
