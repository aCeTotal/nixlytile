#ifndef PLATFORM_BACKEND_BACKEND_H
#define PLATFORM_BACKEND_BACKEND_H

#include <wayland-server-core.h>

struct nx_session;
struct nx_backend_impl;

// Source of input and output devices.
struct nx_backend {
	const struct nx_backend_impl *impl;

	// Bitfield of enum nx_buffer_cap.
	uint32_t buffer_caps;

	struct {
		// Commits accept wait/signal timelines.
		bool timeline;
	} features;

	struct {
		struct wl_signal destroy;
		struct wl_signal new_input; // struct nx_input_device
		struct wl_signal new_output; // struct nx_output
	} events;
};

// Multi backend picked from the environment.
struct nx_backend *nx_backend_autocreate(struct wl_event_loop *loop,
	struct nx_session **session_ptr);
bool nx_backend_start(struct nx_backend *backend);
void nx_backend_destroy(struct nx_backend *backend);
// Still owned by the backend.
int nx_backend_get_drm_fd(struct nx_backend *backend);
// Handles input already queued.
void nx_backend_dispatch_input(struct nx_backend *backend);

#endif
