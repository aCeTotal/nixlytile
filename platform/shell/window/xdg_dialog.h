#ifndef PLATFORM_SHELL_WINDOW_XDG_DIALOG_H
#define PLATFORM_SHELL_WINDOW_XDG_DIALOG_H

#include <wayland-server-core.h>
#include "platform/container/addon.h"

struct nx_xdg_toplevel;

// Modality is accepted and ignored.
struct nx_xdg_dialog_v1 {
	struct wl_resource *resource;

	struct {
		struct nx_addon surface_addon;

		struct wl_listener xdg_toplevel_destroy;
	};
};

struct nx_xdg_wm_dialog_v1 {
	struct wl_global *global;

	struct {
		struct wl_listener display_destroy;
	};
};

struct nx_xdg_wm_dialog_v1 *nx_xdg_wm_dialog_v1_create(struct wl_display *display,
	uint32_t version);

// NULL for non-dialog toplevels.
struct nx_xdg_dialog_v1 *nx_xdg_dialog_v1_try_from_nx_xdg_toplevel(
	struct nx_xdg_toplevel *xdg_toplevel);

#endif
