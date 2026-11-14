#ifndef PLATFORM_CAPTURE_IMAGE_COPY_H
#define PLATFORM_CAPTURE_IMAGE_COPY_H

#include <wayland-server-core.h>

struct nx_ext_image_copy_capture_manager_v1 {
	struct wl_global *global;

	struct {
		struct wl_listener display_destroy;
	};
};

struct nx_ext_image_copy_capture_manager_v1 *nx_ext_image_copy_capture_manager_v1_create(
	struct wl_display *display, uint32_t version);

#endif
