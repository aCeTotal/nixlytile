#ifndef PLATFORM_CLIPBOARD_CONTROL_DATA_CONTROL_V1_H
#define PLATFORM_CLIPBOARD_CONTROL_DATA_CONTROL_V1_H

#include <wayland-server-core.h>

// Legacy twin of ext-data-control.
struct nx_data_control_manager_v1 {
	struct wl_global *global;

	struct {
		struct wl_listener display_destroy;
	};
};

struct nx_data_control_manager_v1 *nx_data_control_manager_v1_create(
	struct wl_display *display);

#endif
