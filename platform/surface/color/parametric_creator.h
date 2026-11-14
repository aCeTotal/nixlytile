#ifndef PLATFORM_SURFACE_COLOR_PARAMETRIC_CREATOR_H
#define PLATFORM_SURFACE_COLOR_PARAMETRIC_CREATOR_H

#include "platform/surface/color_management.h"

struct color_params {
	struct nx_color_manager_v1 *manager;
	struct nx_image_description_v1_data data;
};

struct color_params *color_params_from_resource(struct wl_resource *resource);
struct nx_color_cie1931_xy color_params_decode_xy(int32_t raw_x, int32_t raw_y);

void color_params_set_mastering_display_primaries(struct wl_client *client,
	struct wl_resource *params_resource,
	int32_t r_x, int32_t r_y, int32_t g_x, int32_t g_y,
	int32_t b_x, int32_t b_y, int32_t w_x, int32_t w_y);
void color_params_set_mastering_luminance(struct wl_client *client,
	struct wl_resource *params_resource, uint32_t min_lum, uint32_t max_lum);
void color_params_set_max_cll(struct wl_client *client,
	struct wl_resource *params_resource, uint32_t max_cll);
void color_params_set_max_fall(struct wl_client *client,
	struct wl_resource *params_resource, uint32_t max_fall);

#endif
