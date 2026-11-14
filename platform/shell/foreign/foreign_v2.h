#ifndef PLATFORM_SHELL_FOREIGN_FOREIGN_V2_H
#define PLATFORM_SHELL_FOREIGN_FOREIGN_V2_H

#include "platform/shell/foreign/xdg_foreign.h"
#include "platform/shell/xdg/xdg_shell.h"

void shell_foreign_exporter_bind(struct wl_client *client, void *data,
	uint32_t version, uint32_t id);
void shell_foreign_importer_bind(struct wl_client *client, void *data,
	uint32_t version, uint32_t id);

// Posts invalid_surface for non-toplevels.
struct nx_xdg_toplevel *shell_foreign_toplevel_get(struct wl_resource *resource,
	struct nx_surface *surface);

#endif
