#ifndef PLATFORM_BACKEND_HEADLESS_HEADLESS_H
#define PLATFORM_BACKEND_HEADLESS_HEADLESS_H

#include "platform/backend/backend.h"
#include "platform/output/output.h"

// Virtual outputs, no inputs.
struct nx_backend *nx_headless_backend_create(struct wl_event_loop *loop);
struct nx_output *nx_headless_add_output(struct nx_backend *backend,
	unsigned int width, unsigned int height);
bool nx_output_is_headless(struct nx_output *output);

#endif
