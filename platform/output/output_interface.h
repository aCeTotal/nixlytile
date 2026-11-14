#ifndef PLATFORM_OUTPUT_OUTPUT_INTERFACE_H
#define PLATFORM_OUTPUT_OUTPUT_INTERFACE_H

#include <stdbool.h>
#include "platform/backend/backend.h"
#include "platform/output/output.h"

// State backends may ignore.
#define NX_OUTPUT_STATE_BACKEND_OPTIONAL \
	(NX_OUTPUT_STATE_DAMAGE | \
	NX_OUTPUT_STATE_SCALE | \
	NX_OUTPUT_STATE_TRANSFORM | \
	NX_OUTPUT_STATE_RENDER_FORMAT | \
	NX_OUTPUT_STATE_SUBPIXEL)

struct nx_output_cursor_size {
	int width, height;
};

// Backend hooks; only commit is mandatory.
struct nx_output_impl {
	// NULL buffer hides the cursor.
	bool (*set_cursor)(struct nx_output *output, struct nx_buffer *buffer,
		int hotspot_x, int hotspot_y);
	// Position of the hotspot.
	bool (*move_cursor)(struct nx_output *output, int x, int y);
	void (*destroy)(struct nx_output *output);
	// Dry run of commit.
	bool (*test)(struct nx_output *output, const struct nx_output_state *state);
	// Buffers schedule a frame.
	bool (*commit)(struct nx_output *output, const struct nx_output_state *state);
	// Zero without gamma LUT support.
	size_t (*get_gamma_size)(struct nx_output *output);
	// Unset accepts any; NULL none.
	const struct nx_drm_format_set *(*get_cursor_formats)(
		struct nx_output *output, uint32_t buffer_caps);
	const struct nx_output_cursor_size *(*get_cursor_sizes)(struct nx_output *output,
		size_t *len);
	// Unset accepts any; NULL none.
	const struct nx_drm_format_set *(*get_primary_formats)(
		struct nx_output *output, uint32_t buffer_caps);
};

void nx_output_init(struct nx_output *output, struct nx_backend *backend,
	const struct nx_output_impl *impl, struct wl_event_loop *event_loop,
	const struct nx_output_state *state);
// Emits destroy and frees common state.
void nx_output_finish(struct nx_output *output);
void nx_output_update_needs_frame(struct nx_output *output);
void nx_output_send_frame(struct nx_output *output);
void nx_output_send_present(struct nx_output *output,
	struct nx_output_event_present *event);
void nx_output_send_request_state(struct nx_output *output,
	const struct nx_output_state *state);

#endif
