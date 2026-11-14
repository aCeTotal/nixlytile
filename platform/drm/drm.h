#ifndef PLATFORM_DRM_DRM_H
#define PLATFORM_DRM_DRM_H

#include <wayland-server-core.h>
#include "platform/backend/backend.h"
#include "platform/session/session.h"
#include "platform/output/output.h"

typedef struct _drmModeModeInfo drmModeModeInfo;

/* Parent must be a DRM backend. */
struct nx_backend *nx_drm_backend_create(struct nx_session *session,
	struct nx_device *dev, struct nx_backend *parent);

bool nx_backend_is_drm(struct nx_backend *backend);
bool nx_output_is_drm(struct nx_output *output);

struct nx_backend *nx_drm_backend_get_parent(struct nx_backend *backend);

uint32_t nx_drm_connector_get_id(struct nx_output *output);

/* Only accepts DRM_MODE_TYPE_USERDEF modes. */
struct nx_output_mode *nx_drm_connector_add_mode(struct nx_output *output,
	const drmModeModeInfo *mode);

/* Base timings at refresh / divisor. */
struct nx_output_mode *nx_drm_connector_add_paced_mode(
	struct nx_output *output, struct nx_output_mode *base, int divisor);

/* Zero when unpaced or non-DRM. */
int nx_drm_connector_mode_pace_divisor(struct nx_output *output,
	struct nx_output_mode *mode);

#endif
