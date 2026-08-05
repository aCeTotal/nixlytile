/*
 * This an unstable interface of wlroots. No guarantees are made regarding the
 * future consistency of this API.
 */
#ifndef WLR_USE_UNSTABLE
#error "Add -DWLR_USE_UNSTABLE to enable unstable wlroots features"
#endif

#ifndef WLR_TYPES_WLR_COMMIT_TIMING_V1_H
#define WLR_TYPES_WLR_COMMIT_TIMING_V1_H

#include <wayland-server-core.h>

struct wlr_commit_timing_manager_v1 {
	struct wl_global *global;

	struct {
		struct wl_listener display_destroy;
	} WLR_PRIVATE;
};

/**
 * Advertise commit timing support to clients.
 *
 * Clients can attach a target presentation time to a surface commit. The
 * commit is held back (cached) until that time is reached, so content is
 * never shown earlier than requested.
 *
 * Timestamps are in the compositor's presentation clock domain, which is
 * assumed to be CLOCK_MONOTONIC.
 */
struct wlr_commit_timing_manager_v1 *wlr_commit_timing_manager_v1_create(
	struct wl_display *display, uint32_t version);

#endif
