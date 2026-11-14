#ifndef PLATFORM_SURFACE_SYNC_LINUX_DRM_SYNCOBJ_H
#define PLATFORM_SURFACE_SYNC_LINUX_DRM_SYNCOBJ_H

#include <stdbool.h>
#include <wayland-server-core.h>

struct nx_drm_syncobj_timeline;
struct nx_surface;

struct nx_drm_syncobj_timeline *syncobj_timeline_from_resource(
	struct wl_resource *resource);
void syncobj_manager_get_surface(struct wl_client *client,
	struct wl_resource *resource, uint32_t id,
	struct wl_resource *surface_resource);
// Holds pending until signalled
bool syncobj_hold_until_acquired(struct nx_surface *surface,
	struct nx_drm_syncobj_timeline *timeline, uint64_t point);

#endif
