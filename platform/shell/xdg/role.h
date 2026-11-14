#ifndef PLATFORM_SHELL_XDG_ROLE_H
#define PLATFORM_SHELL_XDG_ROLE_H

#include "platform/shell/xdg/xdg_shell.h"
#include "xdg-shell-protocol.h"

struct nx_xdg_positioner;

extern const struct xdg_toplevel_interface shell_toplevel_impl;

bool shell_xdg_surface_set_role(struct nx_xdg_surface *surface,
	enum nx_xdg_surface_role role);
void shell_xdg_surface_set_role_object(struct nx_xdg_surface *surface,
	struct wl_resource *role_resource);
void shell_xdg_surface_reset_role_object(struct nx_xdg_surface *surface);
void shell_xdg_surface_destroy_role_object(struct nx_xdg_surface *surface);

void shell_toplevel_create(struct nx_xdg_surface *surface, uint32_t id);
void shell_toplevel_reset(struct nx_xdg_toplevel *toplevel);
void shell_toplevel_destroy(struct nx_xdg_toplevel *toplevel);
void shell_toplevel_client_commit(struct nx_xdg_toplevel *toplevel);
struct nx_xdg_toplevel_configure *shell_toplevel_send_configure(
	struct nx_xdg_toplevel *toplevel);
void shell_toplevel_ack_configure(struct nx_xdg_toplevel *toplevel,
	const struct nx_xdg_toplevel_configure *configure);

void shell_popup_create(struct nx_xdg_surface *surface,
	struct nx_xdg_surface *parent, struct nx_xdg_positioner *positioner,
	uint32_t id);
void shell_popup_reset(struct nx_xdg_popup *popup);
void shell_popup_destroy(struct nx_xdg_popup *popup);
void shell_popup_client_commit(struct nx_xdg_popup *popup);
struct nx_xdg_popup_configure *shell_popup_send_configure(
	struct nx_xdg_popup *popup);
void shell_popup_ack_configure(struct nx_xdg_popup *popup,
	const struct nx_xdg_popup_configure *configure);

#endif
