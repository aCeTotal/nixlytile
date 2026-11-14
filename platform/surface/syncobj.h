#ifndef PLATFORM_SURFACE_SYNCOBJ_H
#define PLATFORM_SURFACE_SYNCOBJ_H

#include <wayland-server-core.h>

struct nx_buffer;
struct nx_surface;
struct nx_linux_drm_syncobj_manager_v1;

struct nx_linux_drm_syncobj_surface_v1_state {
	struct nx_drm_syncobj_timeline *acquire_timeline;
	uint64_t acquire_point;

	struct {
		bool committed;
		struct nx_drm_syncobj_timeline *release_timeline;
		uint64_t release_point;
		struct nx_drm_syncobj_merger *release_merger;
	};
};

// Needs explicit-sync renderer and backend
struct nx_linux_drm_syncobj_manager_v1 *nx_linux_drm_syncobj_manager_v1_create(
	struct wl_display *display, uint32_t version, int drm_fd);

struct nx_linux_drm_syncobj_surface_v1_state *nx_linux_drm_syncobj_v1_get_surface_state(
	struct nx_surface *surface);

// Signals on buffer release
bool nx_linux_drm_syncobj_v1_state_signal_release_with_buffer(
	struct nx_linux_drm_syncobj_surface_v1_state *state, struct nx_buffer *buffer);

// Release waits for all points
bool nx_linux_drm_syncobj_v1_state_add_release_point(
	struct nx_linux_drm_syncobj_surface_v1_state *state,
	struct nx_drm_syncobj_timeline *release_timeline, uint64_t release_point,
	struct wl_event_loop *event_loop);

#endif
