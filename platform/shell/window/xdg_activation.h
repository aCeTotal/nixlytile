#ifndef PLATFORM_SHELL_WINDOW_XDG_ACTIVATION_H
#define PLATFORM_SHELL_WINDOW_XDG_ACTIVATION_H

#include <wayland-server-core.h>

struct nx_xdg_activation_v1 {
	struct wl_global *global;

	struct wl_list tokens; // activation_token.link

	struct {
		struct wl_signal request_activate; // struct nx_xdg_activation_v1_request_activate_event
	} events;

	struct {
		struct wl_display *display;

		struct wl_listener display_destroy;
	};
};

struct nx_xdg_activation_v1_request_activate_event {
	struct nx_surface *surface;
};

struct nx_xdg_activation_v1 *nx_xdg_activation_v1_create(
	struct wl_display *display);

#endif
