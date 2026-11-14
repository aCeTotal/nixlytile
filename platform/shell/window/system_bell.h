#ifndef PLATFORM_SHELL_WINDOW_SYSTEM_BELL_H
#define PLATFORM_SHELL_WINDOW_SYSTEM_BELL_H

#include <wayland-server-core.h>

// Rings are accepted and ignored.
struct nx_xdg_system_bell_v1 {
	struct wl_global *global;

	struct {
		struct wl_listener display_destroy;
	};
};

struct nx_xdg_system_bell_v1 *nx_xdg_system_bell_v1_create(struct wl_display *display,
	uint32_t version);

#endif
