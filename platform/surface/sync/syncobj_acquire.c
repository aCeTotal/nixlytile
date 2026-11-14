#include <stdlib.h>
#include <xf86drm.h>
#include "platform/render/drm_syncobj.h"
#include "platform/surface/compositor.h"
#include "platform/surface/sync/linux_drm_syncobj.h"

struct acquire_hold {
	struct nx_surface *surface;
	struct nx_drm_syncobj_timeline_waiter waiter;
	uint32_t cached_seq;

	struct wl_listener surface_destroy;
};

static void acquire_hold_destroy(struct acquire_hold *hold) {
	nx_surface_unlock_cached(hold->surface, hold->cached_seq);
	wl_list_remove(&hold->surface_destroy.link);
	nx_drm_syncobj_timeline_waiter_finish(&hold->waiter);
	free(hold);
}

static void acquire_hold_handle_waiter_ready(struct nx_drm_syncobj_timeline_waiter *waiter) {
	struct acquire_hold *hold = wl_container_of(waiter, hold, waiter);
	acquire_hold_destroy(hold);
}

static void acquire_hold_handle_surface_destroy(struct wl_listener *listener,
		void *data) {
	struct acquire_hold *hold = wl_container_of(listener, hold, surface_destroy);
	acquire_hold_destroy(hold);
}

// Slow clients never stall composition
bool syncobj_hold_until_acquired(struct nx_surface *surface,
		struct nx_drm_syncobj_timeline *timeline, uint64_t point) {
	bool already_signaled = false;
	if (!nx_drm_syncobj_timeline_check(timeline, point,
			DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT, &already_signaled)) {
		return false;
	}
	if (already_signaled) {
		return true;
	}

	struct acquire_hold *hold = calloc(1, sizeof(*hold));
	if (hold == NULL) {
		return false;
	}

	struct wl_client *client = wl_resource_get_client(surface->resource);
	struct wl_display *display = wl_client_get_display(client);
	struct wl_event_loop *loop = wl_display_get_event_loop(display);
	if (!nx_drm_syncobj_timeline_waiter_init(&hold->waiter, timeline, point,
			0, loop, acquire_hold_handle_waiter_ready)) {
		free(hold);
		return false;
	}

	hold->surface = surface;
	hold->cached_seq = nx_surface_lock_pending(surface);

	hold->surface_destroy.notify = acquire_hold_handle_surface_destroy;
	wl_signal_add(&surface->events.destroy, &hold->surface_destroy);

	return true;
}
