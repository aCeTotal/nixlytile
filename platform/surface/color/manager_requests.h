#ifndef PLATFORM_SURFACE_COLOR_MANAGER_REQUESTS_H
#define PLATFORM_SURFACE_COLOR_MANAGER_REQUESTS_H

#include "platform/surface/color_management.h"

struct nx_color_manager_v1 *color_manager_from_resource(struct wl_resource *resource);

void color_manager_get_output(struct wl_client *client,
	struct wl_resource *manager_resource, uint32_t id,
	struct wl_resource *output_resource);
void color_manager_get_surface(struct wl_client *client,
	struct wl_resource *manager_resource, uint32_t id,
	struct wl_resource *surface_resource);
void color_manager_get_surface_feedback(struct wl_client *client,
	struct wl_resource *manager_resource, uint32_t id,
	struct wl_resource *surface_resource);
void color_manager_create_icc_creator(struct wl_client *client,
	struct wl_resource *manager_resource, uint32_t id);
void color_manager_create_parametric_creator(struct wl_client *client,
	struct wl_resource *manager_resource, uint32_t id);

#endif
