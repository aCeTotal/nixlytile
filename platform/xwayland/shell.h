#ifndef XWAYLAND_SHELL_H
#define XWAYLAND_SHELL_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>

struct nx_surface;

// xwayland_shell_v1, for Xwayland only.
struct nx_xwayland_shell_v1 {
	struct wl_global *global;

	struct {
		struct wl_signal destroy;
		struct wl_signal new_surface; // struct nx_xwayland_surface_v1
	} events;

	struct wl_client *client;
	struct wl_list surfaces; // nx_xwayland_surface_v1.link

	struct wl_listener display_destroy;
	struct wl_listener client_destroy;
};

struct nx_xwayland_surface_v1 {
	struct nx_surface *surface;
	uint64_t serial;

	struct wl_resource *resource;
	struct wl_list link;
	struct nx_xwayland_shell_v1 *shell;
	bool added;
};

struct nx_xwayland_shell_v1 *nx_xwayland_shell_v1_create(
	struct wl_display *display);

void nx_xwayland_shell_v1_destroy(struct nx_xwayland_shell_v1 *shell);

void nx_xwayland_shell_v1_set_client(struct nx_xwayland_shell_v1 *shell,
	struct wl_client *client);

// NULL until a surface claims it.
struct nx_surface *nx_xwayland_shell_v1_surface_from_serial(
	struct nx_xwayland_shell_v1 *shell, uint64_t serial);

#endif
