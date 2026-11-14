#ifndef PLATFORM_OUTPUT_XDG_OUTPUT_H
#define PLATFORM_OUTPUT_XDG_OUTPUT_H

#include <wayland-server-core.h>
#include "platform/output/layout.h"

struct nx_xdg_output_manager_v1;

struct nx_xdg_output_manager_v1 *nx_xdg_output_manager_v1_create(
	struct wl_display *display, struct nx_output_layout *layout);

#endif
