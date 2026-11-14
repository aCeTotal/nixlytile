#ifndef PLATFORM_CAPTURE_SOURCE_OUTPUT_SOURCE_H
#define PLATFORM_CAPTURE_SOURCE_OUTPUT_SOURCE_H

#include <time.h>
#include "platform/capture/source/capture_source.h"

struct output_frame_event {
	struct capture_source_frame_event base;
	struct nx_buffer *buffer;
	struct timespec when;
};

// One output's hardware cursor.
struct output_cursor_source {
	struct capture_cursor_source base;

	struct nx_output *output;
	struct nx_buffer *prev_buffer;
	bool initialized;
	bool needs_frame;

	struct wl_listener output_commit;
	struct wl_listener prev_buffer_release;
};

// Caller finishes it with output.
void capture_output_cursor_init(struct output_cursor_source *cursor_source,
	struct nx_output *output);
void capture_output_cursor_finish(struct output_cursor_source *cursor_source);

#endif
