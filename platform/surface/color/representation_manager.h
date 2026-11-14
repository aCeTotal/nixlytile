#ifndef PLATFORM_SURFACE_COLOR_REPRESENTATION_MANAGER_H
#define PLATFORM_SURFACE_COLOR_REPRESENTATION_MANAGER_H

#include <stdbool.h>
#include "platform/surface/color_representation.h"

// Sole supported alpha mode
#define COLOR_REPR_ALPHA_MODE \
	WP_COLOR_REPRESENTATION_SURFACE_V1_ALPHA_MODE_PREMULTIPLIED_ELECTRICAL

struct nx_color_representation_manager_v1 *color_repr_manager_from_resource(
	struct wl_resource *resource);
bool color_repr_manager_supports(const struct nx_color_representation_manager_v1 *manager,
	uint32_t coefficients, uint32_t range);
void color_repr_manager_get_surface(struct wl_client *client,
	struct wl_resource *manager_resource, uint32_t id,
	struct wl_resource *surface_resource);

#endif
