#include <assert.h>
#include "platform/backend/backend.h"
#include "platform/render/allocator/allocator.h"
#include "platform/render/swapchain.h"
#include "platform/log/log.h"
#include "platform/output/commit/commit.h"

bool nx_output_init_render(struct nx_output *output,
		struct nx_allocator *allocator, struct nx_renderer *renderer) {
	assert(allocator != NULL && renderer != NULL);

	if (!(output->backend->buffer_caps & allocator->buffer_caps)) {
		nx_log(NX_ERROR, "output backend and allocator buffer capabilities "
			"don't match");
		return false;
	}
	if (!(renderer->render_buffer_caps & allocator->buffer_caps)) {
		nx_log(NX_ERROR, "renderer and allocator buffer capabilities "
			"don't match");
		return false;
	}

	nx_swapchain_destroy(output->swapchain);
	output->swapchain = NULL;

	nx_swapchain_destroy(output->cursor_swapchain);
	output->cursor_swapchain = NULL;

	output->allocator = allocator;
	output->renderer = renderer;
	return true;
}

static struct nx_buffer *output_acquire_empty_buffer(struct nx_output *output,
		const struct nx_output_state *state) {
	assert(!(state->committed & NX_OUTPUT_STATE_BUFFER));

	// Buffered swapchain test cannot recurse.
	if (!nx_output_configure_primary_swapchain(output, state,
			&output->swapchain)) {
		return NULL;
	}

	struct nx_buffer *buffer = nx_swapchain_acquire(output->swapchain);
	if (buffer == NULL) {
		return NULL;
	}

	struct nx_render_pass *pass =
		nx_renderer_begin_buffer_pass(output->renderer, buffer, NULL);
	if (pass == NULL) {
		nx_buffer_unlock(buffer);
		return NULL;
	}

	nx_render_pass_add_rect(pass, &(struct nx_render_rect_options){
		.color = { 0, 0, 0, 0 },
		.blend_mode = NX_RENDER_BLEND_MODE_NONE,
	});

	if (!nx_render_pass_submit(pass)) {
		nx_buffer_unlock(buffer);
		return NULL;
	}
	return buffer;
}

static bool needs_modeset_buffer(struct nx_output *output,
		const struct nx_output_state *state) {
	if ((state->committed & NX_OUTPUT_STATE_ENABLED) && state->enabled) {
		return true;
	}
	if (state->committed & (NX_OUTPUT_STATE_MODE | NX_OUTPUT_STATE_RENDER_FORMAT)) {
		return true;
	}
	// First commit creates the swapchain.
	return state->allow_reconfiguration && output->commit_seq == 0 &&
		output_pending_enabled(output, state);
}

bool output_ensure_buffer(struct nx_output *output,
		struct nx_output_state *state, bool *new_buffer) {
	assert(*new_buffer == false);

	// Compositor attaches buffers without renderer.
	if ((state->committed & NX_OUTPUT_STATE_BUFFER) || output->renderer == NULL) {
		return true;
	}
	if (!needs_modeset_buffer(output, state)) {
		return true;
	}

	nx_log(NX_DEBUG, "Attaching empty buffer to output for modeset");
	struct nx_buffer *buffer = output_acquire_empty_buffer(output, state);
	if (buffer == NULL) {
		return false;
	}

	*new_buffer = true;
	nx_output_state_set_buffer(state, buffer);
	nx_buffer_unlock(buffer);
	return true;
}

void nx_output_lock_attach_render(struct nx_output *output, bool lock) {
	if (lock) {
		++output->attach_render_locks;
	} else {
		assert(output->attach_render_locks > 0);
		--output->attach_render_locks;
	}
	nx_log(NX_DEBUG, "%s direct scan-out on output '%s' (locks: %d)",
		lock ? "Disabling" : "Enabling", output->name,
		output->attach_render_locks);
}

bool nx_output_is_direct_scanout_allowed(struct nx_output *output) {
	if (output->attach_render_locks > 0) {
		nx_log(NX_DEBUG, "Direct scan-out disabled by lock");
		return false;
	}

	struct nx_output_cursor *cursor;
	wl_list_for_each(cursor, &output->cursors, link) {
		if (cursor->enabled && cursor->visible &&
				cursor != output->hardware_cursor) {
			nx_log(NX_DEBUG, "Direct scan-out disabled by software cursor");
			return false;
		}
	}
	return true;
}
