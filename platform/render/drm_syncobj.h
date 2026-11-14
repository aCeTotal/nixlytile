#ifndef PLATFORM_RENDER_DRM_SYNCOBJ_H
#define PLATFORM_RENDER_DRM_SYNCOBJ_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>
#include "platform/container/addon.h"

/* Monotonic 64-bit drm_syncobj points */
struct nx_drm_syncobj_timeline {
	int drm_fd;
	uint32_t handle;

	struct nx_addon_set addons;

	struct {
		size_t n_refs;
	};
};

struct nx_drm_syncobj_timeline_waiter;

typedef void (*nx_drm_syncobj_timeline_ready_callback)(
	struct nx_drm_syncobj_timeline_waiter *waiter);

struct nx_drm_syncobj_timeline_waiter {
	struct {
		int ev_fd;
		struct wl_event_source *event_source;
		nx_drm_syncobj_timeline_ready_callback callback;
	};
};

struct nx_drm_syncobj_timeline *nx_drm_syncobj_timeline_create(int drm_fd);
struct nx_drm_syncobj_timeline *nx_drm_syncobj_timeline_import(int drm_fd,
	int drm_syncobj_fd);
struct nx_drm_syncobj_timeline *nx_drm_syncobj_timeline_ref(struct nx_drm_syncobj_timeline *timeline);
void nx_drm_syncobj_timeline_unref(struct nx_drm_syncobj_timeline *timeline);
int nx_drm_syncobj_timeline_export(struct nx_drm_syncobj_timeline *timeline);

/* Flags: DRM_SYNCOBJ_WAIT_FLAGS_WAIT_{FOR_SUBMIT,AVAILABLE} */
bool nx_drm_syncobj_timeline_check(struct nx_drm_syncobj_timeline *timeline,
	uint64_t point, uint32_t flags, bool *result);
bool nx_drm_syncobj_timeline_signal(struct nx_drm_syncobj_timeline *timeline, uint64_t point);

/* Callback fires from loop */
bool nx_drm_syncobj_timeline_waiter_init(struct nx_drm_syncobj_timeline_waiter *waiter,
	struct nx_drm_syncobj_timeline *timeline, uint64_t point, uint32_t flags,
	struct wl_event_loop *loop, nx_drm_syncobj_timeline_ready_callback callback);
void nx_drm_syncobj_timeline_waiter_finish(struct nx_drm_syncobj_timeline_waiter *waiter);

/* Point must already be materialized */
int nx_drm_syncobj_timeline_export_sync_file(struct nx_drm_syncobj_timeline *timeline,
	uint64_t src_point);
bool nx_drm_syncobj_timeline_import_sync_file(struct nx_drm_syncobj_timeline *timeline,
	uint64_t dst_point, int sync_file_fd);

#endif
