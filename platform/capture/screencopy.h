#ifndef PLATFORM_CAPTURE_SCREENCOPY_H
#define PLATFORM_CAPTURE_SCREENCOPY_H

#include <wayland-server-core.h>

// Legacy twin of image-copy-capture.
struct nx_screencopy_manager_v1 {
	struct wl_global *global;

	struct {
		struct wl_listener display_destroy;
	};
};

struct nx_screencopy_manager_v1 *nx_screencopy_manager_v1_create(
	struct wl_display *display);

#endif
