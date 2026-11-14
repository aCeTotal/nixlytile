#ifndef PLATFORM_RENDER_DRM_SYNCOBJ_MERGER_H
#define PLATFORM_RENDER_DRM_SYNCOBJ_MERGER_H

#include <wayland-server-core.h>

/* Joins input points into one */
struct nx_drm_syncobj_merger {
	int n_ref;
	struct nx_drm_syncobj_timeline *dst_timeline;
	uint64_t dst_point;
	int sync_fd;
};

struct nx_drm_syncobj_merger *nx_drm_syncobj_merger_create(
	struct nx_drm_syncobj_timeline *dst_timeline, uint64_t dst_point);

struct nx_drm_syncobj_merger *nx_drm_syncobj_merger_ref(
	struct nx_drm_syncobj_merger *merger);

/* Last unref materializes dst point */
void nx_drm_syncobj_merger_unref(struct nx_drm_syncobj_merger *merger);

/* Unmaterialized points wait on loop */
bool nx_drm_syncobj_merger_add(struct nx_drm_syncobj_merger *merger,
	struct nx_drm_syncobj_timeline *dst_timeline, uint64_t dst_point,
	struct wl_event_loop *loop);

#endif
