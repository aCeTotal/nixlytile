#ifndef PLATFORM_BACKEND_HEADLESS_BACKEND_H
#define PLATFORM_BACKEND_HEADLESS_BACKEND_H

#include "platform/backend/headless/headless.h"
#include "platform/backend/backend_interface.h"

struct nx_headless_backend {
	struct nx_backend backend;
	struct wl_event_loop *event_loop;
	struct wl_list outputs; // nx_headless_output.link
	struct wl_listener event_loop_destroy;
	bool started;
};

struct nx_headless_output {
	struct nx_output nx_output;

	struct nx_headless_backend *backend;
	struct wl_list link;

	struct wl_event_source *frame_timer;
	int frame_delay; // ms
};

struct nx_headless_backend *headless_backend_from_backend(
	struct nx_backend *nx_backend);

#endif
