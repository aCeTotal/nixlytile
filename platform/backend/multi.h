#ifndef PLATFORM_BACKEND_MULTI_H
#define PLATFORM_BACKEND_MULTI_H

#include "platform/backend/backend.h"

// Aggregates the events of child backends.
struct nx_backend *nx_multi_backend_create(struct wl_event_loop *loop);
// Add before starting the child.
bool nx_multi_backend_add(struct nx_backend *multi,
	struct nx_backend *backend);
bool nx_backend_is_multi(struct nx_backend *backend);

#endif
