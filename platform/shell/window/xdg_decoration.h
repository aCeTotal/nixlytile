#ifndef PLATFORM_SHELL_WINDOW_XDG_DECORATION_H
#define PLATFORM_SHELL_WINDOW_XDG_DECORATION_H

#include <wayland-server-core.h>
#include "platform/shell/xdg/xdg_shell.h"

// Values match the protocol.
enum nx_xdg_toplevel_decoration_v1_mode {
	NX_XDG_TOPLEVEL_DECORATION_V1_MODE_NONE = 0,
	NX_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE = 2,
};

struct nx_xdg_decoration_manager_v1 {
	struct wl_global *global;
	struct wl_list decorations; // nx_xdg_toplevel_decoration_v1.link

	struct {
		struct wl_signal new_toplevel_decoration; // struct nx_xdg_toplevel_decoration_v1
	} events;

	struct {
		struct wl_listener display_destroy;
	};
};

struct nx_xdg_toplevel_decoration_v1 {
	struct wl_resource *resource;
	struct nx_xdg_toplevel *toplevel;
	struct wl_list link; // nx_xdg_decoration_manager_v1.decorations

	enum nx_xdg_toplevel_decoration_v1_mode acked_mode;
	enum nx_xdg_toplevel_decoration_v1_mode scheduled_mode;

	struct wl_list configure_list; // decoration_configure.link

	struct {
		struct wl_signal destroy;
		struct wl_signal request_mode;
	} events;

	struct {
		struct wl_listener toplevel_destroy;
		struct wl_listener surface_configure;
		struct wl_listener surface_ack_configure;
	};
};

struct nx_xdg_decoration_manager_v1 *
	nx_xdg_decoration_manager_v1_create(struct wl_display *display);

uint32_t nx_xdg_toplevel_decoration_v1_set_mode(
	struct nx_xdg_toplevel_decoration_v1 *decoration,
	enum nx_xdg_toplevel_decoration_v1_mode mode);

#endif
