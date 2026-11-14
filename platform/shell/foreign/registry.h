#ifndef PLATFORM_SHELL_FOREIGN_REGISTRY_H
#define PLATFORM_SHELL_FOREIGN_REGISTRY_H

#include <wayland-server-core.h>

#define NX_XDG_FOREIGN_HANDLE_SIZE 37

// Exported toplevels by handle.
struct nx_xdg_foreign_registry {
	struct wl_list exported_surfaces; // nx_xdg_foreign_exported.link

	struct {
		struct wl_signal destroy;
	} events;

	struct {
		struct wl_listener display_destroy;
	};
};

struct nx_xdg_foreign_exported {
	struct wl_list link; // nx_xdg_foreign_registry.exported_surfaces

	struct nx_xdg_toplevel *toplevel;

	char handle[NX_XDG_FOREIGN_HANDLE_SIZE];

	struct {
		struct wl_signal destroy;
	} events;
};

// Destroyed with the display.
struct nx_xdg_foreign_registry *nx_xdg_foreign_registry_create(
	struct wl_display *display);

// Assigns a unique handle.
bool nx_xdg_foreign_exported_init(struct nx_xdg_foreign_exported *exported,
	struct nx_xdg_foreign_registry *registry);

struct nx_xdg_foreign_exported *nx_xdg_foreign_registry_find_by_handle(
	struct nx_xdg_foreign_registry *registry, const char *handle);

void nx_xdg_foreign_exported_finish(struct nx_xdg_foreign_exported *exported);

#endif
