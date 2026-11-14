#ifndef PLATFORM_DRM_MONITOR_H
#define PLATFORM_DRM_MONITOR_H

#include "platform/drm/drm.h"

/* Adds backends for hotplugged GPUs. */
struct nx_drm_backend_monitor;

struct nx_drm_backend_monitor *drm_backend_monitor_create(
	struct nx_backend *multi, struct nx_backend *primary_drm,
	struct nx_session *session);

#endif
