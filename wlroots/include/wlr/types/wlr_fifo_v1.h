/*
 * This an unstable interface of wlroots. No guarantees are made regarding the
 * future consistency of this API.
 */
#ifndef WLR_USE_UNSTABLE
#error "Add -DWLR_USE_UNSTABLE to enable unstable wlroots features"
#endif

#ifndef WLR_TYPES_WLR_FIFO_V1_H
#define WLR_TYPES_WLR_FIFO_V1_H

#include <wayland-server-core.h>

struct wlr_surface;

struct wlr_fifo_manager_v1 {
	struct wl_global *global;

	struct {
		struct wl_listener display_destroy;
	} WLR_PRIVATE;
};

/**
 * Advertise fifo support to clients.
 *
 * Clients can mark a commit with a fifo barrier and make later commits wait
 * for it. A barrier is cleared once the content it belongs to has been latched
 * for display, which throttles such clients to the display refresh rate
 * without relying on frame callbacks.
 *
 * Compositors must call wlr_fifo_v1_surface_latched() when surface content is
 * latched for display. wlr_scene does this automatically.
 */
struct wlr_fifo_manager_v1 *wlr_fifo_manager_v1_create(
	struct wl_display *display, uint32_t version);

/**
 * Notify that the surface's current content has been latched for display.
 *
 * This clears the surface's fifo barrier and releases the next commit waiting
 * on it. Calling this for a surface without a fifo object is a no-op.
 */
void wlr_fifo_v1_surface_latched(struct wlr_surface *surface);

#endif
