#ifndef PLATFORM_CAPTURE_SOURCE_SOURCE_H
#define PLATFORM_CAPTURE_SOURCE_SOURCE_H

#include <wayland-server-core.h>

// One capture source per output.
struct nx_ext_output_image_capture_source_manager_v1 {
	struct wl_global *global;

	struct {
		struct wl_listener display_destroy;
	};
};

struct nx_ext_output_image_capture_source_manager_v1 *
	nx_ext_output_image_capture_source_manager_v1_create(
	struct wl_display *display, uint32_t version);

#endif
