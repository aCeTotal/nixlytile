#ifndef PLATFORM_CAPTURE_EXPORT_DMABUF_H
#define PLATFORM_CAPTURE_EXPORT_DMABUF_H

#include <wayland-server-core.h>

struct nx_export_dmabuf_manager_v1 {
	struct wl_global *global;

	struct {
		struct wl_listener display_destroy;
	};
};

struct nx_export_dmabuf_manager_v1 *nx_export_dmabuf_manager_v1_create(
	struct wl_display *display);

#endif
