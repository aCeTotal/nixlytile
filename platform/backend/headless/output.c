#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include "platform/output/output_interface.h"
#include "platform/log/log.h"
#include "platform/backend/headless/backend.h"
#include "platform/output/commit/commit.h"

static const int32_t DEFAULT_REFRESH_MHZ = 60 * 1000;

// Image descriptions let remote outputs go HDR.
static const uint32_t SUPPORTED_OUTPUT_STATE =
	NX_OUTPUT_STATE_BACKEND_OPTIONAL |
	NX_OUTPUT_STATE_BUFFER |
	NX_OUTPUT_STATE_ENABLED |
	NX_OUTPUT_STATE_MODE |
	NX_OUTPUT_STATE_IMAGE_DESCRIPTION;

static size_t last_output_num = 0;

static struct nx_headless_output *headless_output_from_output(
		struct nx_output *nx_output) {
	assert(nx_output_is_headless(nx_output));
	struct nx_headless_output *output = wl_container_of(nx_output, output, nx_output);
	return output;
}

static void output_update_refresh(struct nx_headless_output *output,
		int32_t refresh) {
	if (refresh <= 0) {
		refresh = DEFAULT_REFRESH_MHZ;
	}
	output->frame_delay = 1000000 / refresh;
}

static bool output_test(struct nx_output *nx_output,
		const struct nx_output_state *state) {
	uint32_t unsupported = state->committed & ~SUPPORTED_OUTPUT_STATE;
	if (unsupported != 0) {
		nx_log(NX_DEBUG, "Unsupported output state fields: 0x%"PRIx32,
			unsupported);
		return false;
	}

	assert(!(state->committed & NX_OUTPUT_STATE_MODE) ||
		state->mode_type == NX_OUTPUT_STATE_MODE_CUSTOM);
	return true;
}

static bool output_commit(struct nx_output *nx_output,
		const struct nx_output_state *state) {
	struct nx_headless_output *output =
		headless_output_from_output(nx_output);

	if (!output_test(nx_output, state)) {
		return false;
	}

	if (state->committed & NX_OUTPUT_STATE_MODE) {
		output_update_refresh(output, state->custom_mode.refresh);
	}

	if (output_pending_enabled(nx_output, state)) {
		struct nx_output_event_present present_event = {
			.commit_seq = nx_output->commit_seq + 1,
			.presented = true,
		};
		output_defer_present(nx_output, present_event);

		wl_event_source_timer_update(output->frame_timer, output->frame_delay);
	}

	return true;
}

static bool output_set_cursor(struct nx_output *nx_output,
		struct nx_buffer *buffer, int hotspot_x, int hotspot_y) {
	return true;
}

static bool output_move_cursor(struct nx_output *nx_output, int x, int y) {
	return true;
}

static void output_destroy(struct nx_output *nx_output) {
	struct nx_headless_output *output = headless_output_from_output(nx_output);

	nx_output_finish(nx_output);

	wl_list_remove(&output->link);
	wl_event_source_remove(output->frame_timer);
	free(output);
}

static const struct nx_output_impl output_impl = {
	.destroy = output_destroy,
	.test = output_test,
	.commit = output_commit,
	.set_cursor = output_set_cursor,
	.move_cursor = output_move_cursor,
};

bool nx_output_is_headless(struct nx_output *nx_output) {
	return nx_output->impl == &output_impl;
}

static int signal_frame(void *data) {
	struct nx_headless_output *output = data;
	nx_output_send_frame(&output->nx_output);
	return 0;
}

struct nx_output *nx_headless_add_output(struct nx_backend *nx_backend,
		unsigned int width, unsigned int height) {
	struct nx_headless_backend *backend =
		headless_backend_from_backend(nx_backend);

	struct nx_headless_output *output = calloc(1, sizeof(*output));
	if (output == NULL) {
		nx_log(NX_ERROR, "Failed to allocate nx_headless_output");
		return NULL;
	}
	output->backend = backend;
	struct nx_output *nx_output = &output->nx_output;

	struct nx_output_state state;
	nx_output_state_init(&state);
	nx_output_state_set_custom_mode(&state, width, height, 0);
	nx_output_init(nx_output, &backend->backend, &output_impl, backend->event_loop, &state);
	nx_output_state_finish(&state);

	// No EDID, so claim HDR.
	nx_output->supported_primaries |= NX_COLOR_NAMED_PRIMARIES_BT2020;
	nx_output->supported_transfer_functions |= NX_COLOR_TRANSFER_FUNCTION_ST2084_PQ;

	output_update_refresh(output, 0);

	size_t output_num = ++last_output_num;

	char name[64];
	snprintf(name, sizeof(name), "HEADLESS-%zu", output_num);
	nx_output_set_name(nx_output, name);

	char description[128];
	snprintf(description, sizeof(description), "Headless output %zu", output_num);
	nx_output_set_description(nx_output, description);

	output->frame_timer = wl_event_loop_add_timer(backend->event_loop, signal_frame, output);
	wl_list_insert(&backend->outputs, &output->link);

	if (backend->started) {
		wl_signal_emit_mutable(&backend->backend.events.new_output, nx_output);
	}
	return nx_output;
}
