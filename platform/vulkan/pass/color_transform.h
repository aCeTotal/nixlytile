#ifndef NX_VULKAN_PASS_COLOR_TRANSFORM_H
#define NX_VULKAN_PASS_COLOR_TRANSFORM_H

#include <stdbool.h>
#include <wayland-util.h>
#include "platform/container/addon.h"
#include "platform/render/color.h"
#include "platform/vulkan/pass/lut3d.h"

struct nx_vk_renderer;

// Prepared output color transform.
struct nx_vk_color_transform {
	struct nx_addon addon; // owner: nx_vk_renderer
	struct wl_list link; // nx_vk_renderer.color_transforms

	// When used, replaces everything else.
	struct nx_vk_lut3d lut_3d;

	float color_matrix[9];
	enum nx_color_transfer_function inverse_eotf;
};

struct nx_vk_color_transform *vulkan_get_color_transform(
	struct nx_color_transform *transform, struct nx_vk_renderer *renderer);
bool vulkan_ensure_color_transform(struct nx_vk_renderer *renderer,
	struct nx_color_transform *transform);
void vulkan_color_transform_destroy(struct nx_addon *addon);

#endif
