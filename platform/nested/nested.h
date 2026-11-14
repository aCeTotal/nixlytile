#ifndef PLATFORM_NESTED_NESTED_H
#define PLATFORM_NESTED_NESTED_H

#include <wayland-server-core.h>
#include "platform/backend/backend.h"

// Runs as a client of the host compositor.
struct nx_backend *nx_wl_backend_create(struct wl_event_loop *loop);
// Deferred until start; returns NULL then.
struct nx_output *nx_wl_output_create(struct nx_backend *backend);

#endif
