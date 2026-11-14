#ifndef PLATFORM_SHELL_XDG_SURFACE_H
#define PLATFORM_SHELL_XDG_SURFACE_H

#include "platform/shell/xdg/xdg_shell.h"
#include "xdg-shell-protocol.h"

extern const struct nx_surface_role shell_xdg_surface_role;

void shell_xdg_surface_create(struct nx_xdg_client *client,
	struct nx_surface *nx_surface, uint32_t id);
void shell_xdg_surface_destroy(struct nx_xdg_surface *surface);
// Back to the unconfigured state.
void shell_xdg_surface_reset(struct nx_xdg_surface *surface);

// NULL for inert resources.
struct nx_xdg_surface *shell_xdg_surface_from_resource(
	struct wl_resource *resource);

// xdg_surface.ack_configure request.
void shell_xdg_surface_handle_ack_configure(struct wl_client *client,
	struct wl_resource *resource, uint32_t serial);

#endif
