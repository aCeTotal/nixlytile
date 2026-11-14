#ifndef NX_VULKAN_PASS_LUT3D_H
#define NX_VULKAN_PASS_LUT3D_H

#include <stdbool.h>
#include <stddef.h>
#include <vulkan/vulkan.h>

struct nx_vk_renderer;
struct nx_vk_descriptor_pool;
struct nx_color_transform;

// 3D LUT for output.frag.
struct nx_vk_lut3d {
	size_t dim; // zero when unused
	VkImage image;
	VkImageView image_view;
	VkDeviceMemory memory;
	VkDescriptorSet ds;
	struct nx_vk_descriptor_pool *ds_pool;
};

bool vulkan_lut3d_init_dummy(struct nx_vk_renderer *renderer);
bool vulkan_lut3d_bake(struct nx_vk_renderer *renderer, struct nx_vk_lut3d *lut,
	struct nx_color_transform *transform);
void vulkan_lut3d_finish(struct nx_vk_lut3d *lut, VkDevice dev);

#endif
