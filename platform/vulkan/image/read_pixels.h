#ifndef NX_VULKAN_IMAGE_READ_PIXELS_H
#define NX_VULKAN_IMAGE_READ_PIXELS_H

#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

struct nx_vk_renderer;

struct nx_vk_pixel_read {
	VkFormat src_format;
	VkImage src_image;
	uint32_t drm_format;
	uint32_t stride;
	uint32_t width, height;
	uint32_t src_x, src_y;
	void *data;
};

// Reused host-visible copy target.
struct nx_vk_readback_cache {
	bool initialized;
	uint32_t drm_format;
	uint32_t width, height;
	VkImage image;
	VkDeviceMemory memory;
	void *map; // persistent mapping of memory
	VkFormat src_format;
	bool blit_supported; // from src_format into image
};

bool vulkan_read_pixels(struct nx_vk_renderer *renderer, const struct nx_vk_pixel_read *read);
bool vulkan_readback_prepare(struct nx_vk_renderer *renderer,
	const struct nx_vk_pixel_read *read, VkFormat format);
void vulkan_readback_finish(struct nx_vk_renderer *renderer);

#endif
