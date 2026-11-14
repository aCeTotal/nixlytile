#ifndef PLATFORM_SHELL_LOCK_LOCK_SURFACE_H
#define PLATFORM_SHELL_LOCK_LOCK_SURFACE_H

#include "platform/shell/lock/session_lock.h"

// ext_session_lock_v1.get_lock_surface request.
void shell_lock_handle_get_lock_surface(struct wl_client *client,
	struct wl_resource *lock_resource, uint32_t id,
	struct wl_resource *surface_resource,
	struct wl_resource *output_resource);
void shell_lock_surface_destroy(struct nx_session_lock_surface_v1 *lock_surface);

#endif
