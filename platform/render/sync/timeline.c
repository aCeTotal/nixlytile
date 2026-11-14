#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <xf86drm.h>
#include "platform/container/addon.h"
#include "platform/log/log.h"
#include "platform/render/drm_syncobj.h"

static struct nx_drm_syncobj_timeline *timeline_create(int drm_fd, uint32_t handle) {
	struct nx_drm_syncobj_timeline *timeline = calloc(1, sizeof(*timeline));
	if (timeline == NULL) {
		return NULL;
	}

	timeline->drm_fd = drm_fd;
	timeline->n_refs = 1;
	timeline->handle = handle;

	nx_addon_set_init(&timeline->addons);

	return timeline;
}

struct nx_drm_syncobj_timeline *nx_drm_syncobj_timeline_create(int drm_fd) {
	uint32_t handle = 0;
	if (drmSyncobjCreate(drm_fd, 0, &handle) != 0) {
		nx_log_errno(NX_ERROR, "drmSyncobjCreate failed");
		return NULL;
	}

	struct nx_drm_syncobj_timeline *timeline = timeline_create(drm_fd, handle);
	if (timeline == NULL) {
		drmSyncobjDestroy(drm_fd, handle);
	}

	return timeline;
}

struct nx_drm_syncobj_timeline *nx_drm_syncobj_timeline_import(int drm_fd,
		int drm_syncobj_fd) {
	uint32_t handle = 0;
	if (drmSyncobjFDToHandle(drm_fd, drm_syncobj_fd, &handle) != 0) {
		nx_log_errno(NX_ERROR, "drmSyncobjFDToHandle failed");
		return NULL;
	}

	struct nx_drm_syncobj_timeline *timeline = timeline_create(drm_fd, handle);
	if (timeline == NULL) {
		drmSyncobjDestroy(drm_fd, handle);
	}

	return timeline;
}

struct nx_drm_syncobj_timeline *nx_drm_syncobj_timeline_ref(struct nx_drm_syncobj_timeline *timeline) {
	timeline->n_refs++;
	return timeline;
}

void nx_drm_syncobj_timeline_unref(struct nx_drm_syncobj_timeline *timeline) {
	if (timeline == NULL) {
		return;
	}

	assert(timeline->n_refs > 0);
	timeline->n_refs--;
	if (timeline->n_refs > 0) {
		return;
	}

	nx_addon_set_finish(&timeline->addons);
	drmSyncobjDestroy(timeline->drm_fd, timeline->handle);
	free(timeline);
}

int nx_drm_syncobj_timeline_export(struct nx_drm_syncobj_timeline *timeline) {
	int drm_syncobj_fd = -1;
	if (drmSyncobjHandleToFD(timeline->drm_fd, timeline->handle, &drm_syncobj_fd) != 0) {
		nx_log_errno(NX_ERROR, "drmSyncobjHandleToFD failed");
		return -1;
	}
	return drm_syncobj_fd;
}

int nx_drm_syncobj_timeline_export_sync_file(struct nx_drm_syncobj_timeline *timeline,
		uint64_t src_point) {
	int sync_file_fd = -1;

	uint32_t syncobj_handle;
	if (drmSyncobjCreate(timeline->drm_fd, 0, &syncobj_handle) != 0) {
		nx_log_errno(NX_ERROR, "drmSyncobjCreate failed");
		return -1;
	}

	if (drmSyncobjTransfer(timeline->drm_fd, syncobj_handle, 0,
			timeline->handle, src_point, 0) != 0) {
		nx_log_errno(NX_ERROR, "drmSyncobjTransfer failed");
		goto out;
	}

	if (drmSyncobjExportSyncFile(timeline->drm_fd,
			syncobj_handle, &sync_file_fd) != 0) {
		nx_log_errno(NX_ERROR, "drmSyncobjExportSyncFile failed");
		goto out;
	}

out:
	drmSyncobjDestroy(timeline->drm_fd, syncobj_handle);
	return sync_file_fd;
}

bool nx_drm_syncobj_timeline_import_sync_file(struct nx_drm_syncobj_timeline *timeline,
		uint64_t dst_point, int sync_file_fd) {
	bool ok = false;

	uint32_t syncobj_handle;
	if (drmSyncobjCreate(timeline->drm_fd, 0, &syncobj_handle) != 0) {
		nx_log_errno(NX_ERROR, "drmSyncobjCreate failed");
		return false;
	}

	if (drmSyncobjImportSyncFile(timeline->drm_fd, syncobj_handle,
			sync_file_fd) != 0) {
		nx_log_errno(NX_ERROR, "drmSyncobjImportSyncFile failed");
		goto out;
	}

	if (drmSyncobjTransfer(timeline->drm_fd, timeline->handle, dst_point,
			syncobj_handle, 0, 0) != 0) {
		nx_log_errno(NX_ERROR, "drmSyncobjTransfer failed");
		goto out;
	}

	ok = true;

out:
	drmSyncobjDestroy(timeline->drm_fd, syncobj_handle);
	return ok;
}

bool nx_drm_syncobj_timeline_check(struct nx_drm_syncobj_timeline *timeline,
		uint64_t point, uint32_t flags, bool *result) {
	int ret = drmSyncobjTimelineWait(timeline->drm_fd, &timeline->handle, &point, 1, 0, flags, NULL);
	if (ret != 0 && ret != -ETIME) {
		nx_log_errno(NX_ERROR, "drmSyncobjTimelineWait() failed");
		return false;
	}

	*result = ret == 0;
	return true;
}

bool nx_drm_syncobj_timeline_signal(struct nx_drm_syncobj_timeline *timeline, uint64_t point) {
	if (drmSyncobjTimelineSignal(timeline->drm_fd, &timeline->handle, &point, 1) != 0) {
		nx_log(NX_ERROR, "drmSyncobjTimelineSignal() failed");
		return false;
	}
	return true;
}

static int handle_eventfd_ready(int ev_fd, uint32_t mask, void *data) {
	struct nx_drm_syncobj_timeline_waiter *waiter = data;

	if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
		nx_log(NX_ERROR, "Failed to wait for render timeline: eventfd error");
	}

	if (mask & WL_EVENT_READABLE) {
		uint64_t ev_fd_value;
		if (read(ev_fd, &ev_fd_value, sizeof(ev_fd_value)) <= 0) {
			nx_log(NX_ERROR, "Failed to wait for render timeline: read() failed");
		}
	}

	waiter->callback(waiter);
	return 0;
}

bool nx_drm_syncobj_timeline_waiter_init(struct nx_drm_syncobj_timeline_waiter *waiter,
		struct nx_drm_syncobj_timeline *timeline, uint64_t point, uint32_t flags,
		struct wl_event_loop *loop, nx_drm_syncobj_timeline_ready_callback callback) {
	assert(callback);

	int ev_fd = eventfd(0, EFD_CLOEXEC);
	if (ev_fd < 0) {
		nx_log_errno(NX_ERROR, "eventfd() failed");
		return false;
	}

	if (drmSyncobjEventfd(timeline->drm_fd, timeline->handle, point, ev_fd, flags) != 0) {
		nx_log_errno(NX_ERROR, "drmSyncobjEventfd() failed");
		close(ev_fd);
		return false;
	}

	struct wl_event_source *source = wl_event_loop_add_fd(loop, ev_fd,
		WL_EVENT_READABLE, handle_eventfd_ready, waiter);
	if (source == NULL) {
		nx_log(NX_ERROR, "Failed to add FD to event loop");
		close(ev_fd);
		return false;
	}

	*waiter = (struct nx_drm_syncobj_timeline_waiter){
		.ev_fd = ev_fd,
		.event_source = source,
		.callback = callback,
	};
	return true;
}

void nx_drm_syncobj_timeline_waiter_finish(struct nx_drm_syncobj_timeline_waiter *waiter) {
	wl_event_source_remove(waiter->event_source);
	close(waiter->ev_fd);
}
