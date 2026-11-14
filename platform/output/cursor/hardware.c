#include <assert.h>
#include "platform/geometry/transform.h"
#include "platform/log/log.h"
#include "platform/output/output_interface.h"
#include "platform/output/cursor/cursor.h"

static bool output_set_hardware_cursor(struct nx_output *output,
		struct nx_buffer *buffer, int hotspot_x, int hotspot_y) {
	if (!output->impl->set_cursor) {
		return false;
	}
	if (!output->impl->set_cursor(output, buffer, hotspot_x, hotspot_y)) {
		return false;
	}

	nx_output_update_needs_frame(output);

	nx_buffer_unlock(output->cursor_front_buffer);
	output->cursor_front_buffer = buffer != NULL ? nx_buffer_lock(buffer) : NULL;
	return true;
}

bool output_cursor_move_hardware(struct nx_output *output, int x, int y) {
	assert(output->impl->move_cursor);
	if (!output->impl->move_cursor(output, x, y)) {
		return false;
	}
	nx_output_update_needs_frame(output);
	return true;
}

void output_cursor_disable_hardware(struct nx_output *output) {
	if (output->hardware_cursor == NULL) {
		return;
	}

	output_set_hardware_cursor(output, NULL, 0, 0);
	output_cursor_damage_whole(output->hardware_cursor);
	output->hardware_cursor = NULL;
}

void nx_output_lock_software_cursors(struct nx_output *output, bool lock) {
	if (lock) {
		++output->software_cursor_locks;
	} else {
		assert(output->software_cursor_locks > 0);
		--output->software_cursor_locks;
	}
	nx_log(NX_DEBUG, "%s hardware cursors on output '%s' (locks: %d)",
		lock ? "Disabling" : "Enabling", output->name,
		output->software_cursor_locks);

	// Unlock switches back lazily.
	if (output->software_cursor_locks > 0) {
		output_cursor_disable_hardware(output);
	}
}

// Nvidia GBM modifier mismatch fallback.
static bool can_pass_source_buffer(struct nx_output_cursor *cursor) {
	return cursor->source_buffer != NULL &&
		cursor->output->transform == WL_OUTPUT_TRANSFORM_NORMAL;
}

bool output_cursor_attempt_hardware(struct nx_output_cursor *cursor) {
	struct nx_output *output = cursor->output;
	if (!output->impl->set_cursor || output->software_cursor_locks > 0) {
		return false;
	}

	// Position may be stale.
	output_cursor_move_hardware(output, (int)cursor->x, (int)cursor->y);

	struct nx_buffer *buffer = NULL;
	bool from_source = false;
	if (cursor->texture != NULL) {
		buffer = output_cursor_render_buffer(cursor);
		from_source = buffer == NULL && can_pass_source_buffer(cursor);
		if (from_source) {
			buffer = nx_buffer_lock(cursor->source_buffer);
			nx_log(NX_DEBUG, "Cursor render failed, trying source "
				"buffer directly (%dx%d)", buffer->width, buffer->height);
		}
		if (buffer == NULL) {
			nx_log(NX_DEBUG, "Failed to render cursor buffer");
			return false;
		}
	}

	struct nx_box hotspot = {
		.x = cursor->hotspot_x,
		.y = cursor->hotspot_y,
	};
	nx_box_transform(&hotspot, &hotspot,
		nx_output_transform_invert(output->transform),
		buffer ? buffer->width : 0, buffer ? buffer->height : 0);

	bool ok = output_set_hardware_cursor(output, buffer, hotspot.x, hotspot.y);
	if (!ok && from_source) {
		nx_log(NX_DEBUG, "Direct source buffer rejected by HW cursor");
	}
	nx_buffer_unlock(buffer);
	if (ok) {
		output->hardware_cursor = cursor;
	}
	return ok;
}
