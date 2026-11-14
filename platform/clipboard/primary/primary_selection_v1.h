#ifndef PLATFORM_CLIPBOARD_PRIMARY_PRIMARY_SELECTION_V1_H
#define PLATFORM_CLIPBOARD_PRIMARY_PRIMARY_SELECTION_V1_H

#include <wayland-server-core.h>

struct nx_primary_selection_v1_device_manager {
	struct wl_global *global;
	struct wl_list devices; // primary_device.link

	struct {
		struct wl_listener display_destroy;
	};
};

struct nx_primary_selection_v1_device_manager *
	nx_primary_selection_v1_device_manager_create(struct wl_display *display);

#endif
