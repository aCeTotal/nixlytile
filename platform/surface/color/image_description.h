#ifndef PLATFORM_SURFACE_COLOR_IMAGE_DESCRIPTION_H
#define PLATFORM_SURFACE_COLOR_IMAGE_DESCRIPTION_H

#include "platform/surface/color_management.h"

struct color_image_desc_ready {
	struct nx_color_manager_v1 *manager;
	const struct nx_image_description_v1_data *data;
	bool get_info_allowed;
};

struct color_image_desc_failure {
	enum wp_image_description_v1_cause cause;
	const char *msg;
};

void color_image_desc_create_ready(struct wl_resource *parent_resource, uint32_t id,
	const struct color_image_desc_ready *ready);
void color_image_desc_create_failed(struct wl_resource *parent_resource, uint32_t id,
	const struct color_image_desc_failure *failure);
// NULL for failed descriptions
const struct nx_image_description_v1_data *color_image_desc_data_from_resource(
	struct wl_resource *resource);

#endif
