#ifndef PLATFORM_OUTPUT_GAMMA_CONTROL_H
#define PLATFORM_OUTPUT_GAMMA_CONTROL_H

#include <wayland-server-core.h>

struct nx_color_transform;
struct nx_output;
struct nx_gamma_control_v1;

struct nx_gamma_control_manager_v1 {
	struct wl_global *global;
	struct wl_list controls; // nx_gamma_control_v1.link

	struct {
		struct wl_signal destroy;
		struct wl_signal set_gamma; // struct nx_gamma_control_manager_v1_set_gamma_event
	} events;

	struct {
		struct wl_listener display_destroy;
	};
};

struct nx_gamma_control_manager_v1_set_gamma_event {
	struct nx_output *output;
	struct nx_gamma_control_v1 *control; // may be NULL
};

struct nx_gamma_control_manager_v1 *nx_gamma_control_manager_v1_create(
	struct wl_display *display);
struct nx_color_transform *nx_gamma_control_v1_get_color_transform(
	struct nx_gamma_control_v1 *gamma_control);
void nx_gamma_control_v1_send_failed_and_destroy(struct nx_gamma_control_v1 *gamma_control);

#endif
