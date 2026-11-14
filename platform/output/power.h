#ifndef PLATFORM_OUTPUT_POWER_H
#define PLATFORM_OUTPUT_POWER_H

#include <wayland-server-core.h>
#include "wlr-output-power-management-unstable-v1-protocol.h"

struct nx_output_power_manager_v1 {
	struct wl_global *global;
	struct wl_list output_powers; // nx_output_power_v1.link

	struct {
		struct wl_signal set_mode; // struct nx_output_power_v1_set_mode_event
	} events;

	struct {
		struct wl_listener display_destroy;
	};
};

struct nx_output_power_v1_set_mode_event {
	struct nx_output *output;
	enum zwlr_output_power_v1_mode mode;
};

struct nx_output_power_manager_v1 *nx_output_power_manager_v1_create(
	struct wl_display *display);

#endif
