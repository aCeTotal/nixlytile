#include "platform/log/log.h"
#include "platform/output/output_interface.h"
#include "platform/output/commit/commit.h"

bool output_prepare_commit(struct nx_output *output, const struct nx_output_state *state) {
	if (!output_basic_test(output, state)) {
		nx_log(NX_ERROR, "Basic output test failed for %s", output->name);
		return false;
	}

	if ((state->committed & NX_OUTPUT_STATE_BUFFER) && output->idle_frame != NULL) {
		wl_event_source_remove(output->idle_frame);
		output->idle_frame = NULL;
	}

	struct nx_output_event_precommit event = {
		.state = state,
	};
	wl_signal_emit_mutable(&output->events.precommit, &event);
	return true;
}

void output_apply_commit(struct nx_output *output, const struct nx_output_state *state) {
	output->commit_seq++;

	if (output_pending_enabled(output, state)) {
		output->frame_pending = true;
		output->needs_frame = false;
	}

	output_apply_state(output, state);
}

void output_send_commit_event(struct nx_output *output, const struct nx_output_state *state) {
	struct nx_output_event_commit event = {
		.state = state,
	};
	clock_gettime(CLOCK_MONOTONIC, &event.when);
	wl_signal_emit_mutable(&output->events.commit, &event);
}

bool nx_output_commit_state(struct nx_output *output,
		const struct nx_output_state *state) {
	// Commit only what would change.
	struct nx_output_state pending = *state;
	pending.committed &= ~output_compare_state(output, state);

	if (!output_basic_test(output, &pending)) {
		nx_log(NX_ERROR, "Basic output test failed for %s", output->name);
		return false;
	}

	bool new_back_buffer = false;
	if (!output_ensure_buffer(output, &pending, &new_back_buffer)) {
		return false;
	}

	bool ok = output_prepare_commit(output, &pending) &&
		output->impl->commit(output, &pending);
	if (ok) {
		output_apply_commit(output, &pending);
		output_send_commit_event(output, &pending);
	}

	if (new_back_buffer) {
		nx_buffer_unlock(pending.buffer);
	}
	return ok;
}

void nx_output_send_request_state(struct nx_output *output,
		const struct nx_output_state *state) {
	struct nx_output_state copy = *state;
	copy.committed &= ~output_compare_state(output, state);
	if (copy.committed == 0) {
		return;
	}

	struct nx_output_event_request_state event = {
		.output = output,
		.state = &copy,
	};
	wl_signal_emit_mutable(&output->events.request_state, &event);
}
