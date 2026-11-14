#ifndef PLATFORM_CLIPBOARD_CONTROL_EXT_DATA_CONTROL_H
#define PLATFORM_CLIPBOARD_CONTROL_EXT_DATA_CONTROL_H

#include <wayland-server-core.h>

struct nx_ext_data_control_manager_v1 {
	struct wl_global *global;

	struct {
		struct wl_listener display_destroy;
	};
};

struct nx_ext_data_control_manager_v1 *nx_ext_data_control_manager_v1_create(
	struct wl_display *display, uint32_t version);

#endif
