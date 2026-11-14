#ifndef PLATFORM_SHELL_FOREIGN_XDG_FOREIGN_H
#define PLATFORM_SHELL_FOREIGN_XDG_FOREIGN_H

#include <wayland-server-core.h>
#include "platform/shell/foreign/registry.h"

struct nx_xdg_foreign_v2 {
	struct wl_global *exporter_global;
	struct wl_global *importer_global;

	struct nx_xdg_foreign_registry *registry;

	struct {
		struct wl_listener foreign_registry_destroy;
		struct wl_listener display_destroy;
	};
};

struct nx_xdg_foreign_v2 *nx_xdg_foreign_v2_create(
	struct wl_display *display, struct nx_xdg_foreign_registry *registry);

#endif
