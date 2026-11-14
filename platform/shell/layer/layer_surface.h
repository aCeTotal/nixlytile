#ifndef PLATFORM_SHELL_LAYER_LAYER_SURFACE_H
#define PLATFORM_SHELL_LAYER_LAYER_SURFACE_H

#include "platform/shell/layer/layer_shell.h"

extern const struct zwlr_layer_surface_v1_interface shell_layer_surface_impl;

// NULL for inert resources.
struct nx_layer_surface_v1 *shell_layer_surface_from_resource(
	struct wl_resource *resource);
// Back to the unconfigured state.
void shell_layer_surface_reset(struct nx_layer_surface_v1 *surface);
// Destroys without sending closed.
void shell_layer_surface_destroy(struct nx_layer_surface_v1 *surface);

#endif
