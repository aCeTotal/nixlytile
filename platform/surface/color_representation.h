#ifndef PLATFORM_SURFACE_COLOR_REPRESENTATION_H
#define PLATFORM_SURFACE_COLOR_REPRESENTATION_H

#include <wayland-server-core.h>
#include <wayland-protocols/color-representation-v1-enum.h>
#include "platform/render/color.h"

struct nx_surface;
struct nx_renderer;
struct nx_color_representation_manager_v1;

struct nx_color_representation_manager_v1 *nx_color_representation_manager_v1_create_with_renderer(
	struct wl_display *display, uint32_t version, struct nx_renderer *renderer);

struct nx_color_representation_v1_surface_state {
	uint32_t coefficients; // enum wp_color_representation_surface_v1_coefficients, zero if unset
	uint32_t range; // enum wp_color_representation_surface_v1_range, zero if unset
};

const struct nx_color_representation_v1_surface_state *nx_color_representation_v1_get_surface_state(
	struct nx_surface *surface);

enum nx_color_encoding nx_color_representation_v1_color_encoding_to_nx(
	enum wp_color_representation_surface_v1_coefficients wp_val);
enum nx_color_range nx_color_representation_v1_color_range_to_nx(
	enum wp_color_representation_surface_v1_range wp_val);

#endif
