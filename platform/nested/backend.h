#ifndef PLATFORM_NESTED_BACKEND_H
#define PLATFORM_NESTED_BACKEND_H

#include <wayland-client-protocol.h>
#include "platform/backend/backend_interface.h"
#include "platform/nested/nested.h"
#include "platform/render/drm_format_set.h"

struct nx_wl_backend {
	struct nx_backend backend;

	bool started;
	struct wl_event_loop *event_loop;
	// Private queue for blocking roundtrips.
	struct wl_event_queue *busy_loop_queue;
	struct wl_list outputs; // nx_wl_output.link
	int drm_fd;
	struct wl_list buffers; // nx_wl_buffer.link
	size_t requested_outputs;
	struct wl_listener event_loop_destroy;
	char *activation_token;

	struct wl_display *remote_display;
	struct wl_event_source *remote_display_src;
	struct wl_registry *registry;
	struct wl_compositor *compositor;
	struct xdg_wm_base *xdg_wm_base;
	struct zxdg_decoration_manager_v1 *zxdg_decoration_manager_v1;
	struct zwp_pointer_gestures_v1 *zwp_pointer_gestures_v1;
	struct wp_presentation *presentation;
	struct wl_shm *shm;
	struct zwp_linux_dmabuf_v1 *zwp_linux_dmabuf_v1;
	struct wp_linux_drm_syncobj_manager_v1 *drm_syncobj_manager_v1;
	struct wl_list drm_syncobj_timelines; // nx_wl_drm_syncobj_timeline.link
	struct zwp_relative_pointer_manager_v1 *zwp_relative_pointer_manager_v1;
	struct wl_list seats; // nx_wl_seat.link
	struct nx_drm_format_set shm_formats;
	struct nx_drm_format_set linux_dmabuf_v1_formats;
	struct xdg_activation_v1 *activation_v1;
	char *drm_render_name;
};

struct nx_wl_backend *nested_backend_from_backend(struct nx_backend *backend);

extern const struct wl_registry_listener nested_registry_listener;
// Releases every bound host global.
void nested_globals_destroy(struct nx_wl_backend *wl);
// Fills formats and the render node.
bool nested_dmabuf_feedback_query(struct nx_wl_backend *wl);

#endif
