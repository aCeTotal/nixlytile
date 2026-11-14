#ifndef PLATFORM_BUFFER_WL_DRM_H
#define PLATFORM_BUFFER_WL_DRM_H

#include <wayland-server-protocol.h>
#include "platform/render/drm_format_set.h"
#include "platform/buffer/buffer.h"

struct nx_renderer;

/* Legacy Mesa wl_drm, PRIME only */
struct nx_drm *nx_drm_create(struct wl_display *display,
	struct nx_renderer *renderer);

#endif
