#ifndef PLATFORM_SURFACE_SYNC_FIFO_MANAGER_H
#define PLATFORM_SURFACE_SYNC_FIFO_MANAGER_H

#include <wayland-server-core.h>

void fifo_manager_get_fifo(struct wl_client *client,
	struct wl_resource *manager_resource, uint32_t id,
	struct wl_resource *surface_resource);

#endif
