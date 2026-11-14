#ifndef PLATFORM_SHELL_WINDOW_SERVER_DECORATION_H
#define PLATFORM_SHELL_WINDOW_SERVER_DECORATION_H

#include <wayland-server-core.h>

// Matches the KDE protocol value.
enum nx_server_decoration_manager_mode {
	NX_SERVER_DECORATION_MANAGER_MODE_SERVER = 2,
};

// KDE server-side decoration negotiation.
struct nx_server_decoration_manager {
	struct wl_global *global;
	struct wl_list resources; // wl_resource_get_link()

	uint32_t default_mode; // enum nx_server_decoration_manager_mode

	struct {
		struct wl_listener display_destroy;
	};
};

struct nx_server_decoration_manager *nx_server_decoration_manager_create(
	struct wl_display *display);
void nx_server_decoration_manager_set_default_mode(
	struct nx_server_decoration_manager *manager, uint32_t default_mode);

#endif
