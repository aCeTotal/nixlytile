#include <vulkan/vulkan.h>
#include "platform/log/log.h"
#include "platform/vulkan/image/read_pixels.h"
#include "platform/vulkan/renderer.h"

static void release_cache(VkDevice dev, struct nx_vk_readback_cache *cache) {
	vkUnmapMemory(dev, cache->memory);
	vkFreeMemory(dev, cache->memory, NULL);
	vkDestroyImage(dev, cache->image, NULL);
}

static VkImage create_linear_image(VkDevice dev, const struct nx_vk_pixel_read *read,
		VkFormat format) {
	VkImageCreateInfo image_info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = format,
		.extent.width = read->width,
		.extent.height = read->height,
		.extent.depth = 1,
		.arrayLayers = 1,
		.mipLevels = 1,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_LINEAR,
		.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
	};
	VkImage image = VK_NULL_HANDLE;
	VkResult res = vkCreateImage(dev, &image_info, NULL, &image);
	if (res != VK_SUCCESS) {
		nx_vk_error("vkCreateImage", res);
		return VK_NULL_HANDLE;
	}
	return image;
}

// Reuses a matching cached image.
bool vulkan_readback_prepare(struct nx_vk_renderer *renderer,
		const struct nx_vk_pixel_read *read, VkFormat format) {
	struct nx_vk_readback_cache *cache = &renderer->readback;
	if (cache->initialized && cache->drm_format == read->drm_format &&
			cache->width == read->width && cache->height == read->height) {
		return true;
	}

	VkDevice dev = renderer->dev->dev;
	VkImage image = create_linear_image(dev, read, format);
	if (image == VK_NULL_HANDLE) {
		return false;
	}

	VkDeviceMemory memory = vulkan_alloc_image_memory(renderer->dev, image,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
	if (memory == VK_NULL_HANDLE) {
		vkDestroyImage(dev, image, NULL);
		return false;
	}

	// Mapped for the cache lifetime.
	void *map;
	VkResult res = vkMapMemory(dev, memory, 0, VK_WHOLE_SIZE, 0, &map);
	if (res != VK_SUCCESS) {
		nx_vk_error("vkMapMemory", res);
		vkFreeMemory(dev, memory, NULL);
		vkDestroyImage(dev, image, NULL);
		return false;
	}

	if (cache->initialized) {
		release_cache(dev, cache);
	}
	*cache = (struct nx_vk_readback_cache){
		.initialized = true,
		.drm_format = read->drm_format,
		.width = read->width,
		.height = read->height,
		.image = image,
		.memory = memory,
		.map = map,
	};
	return true;
}

void vulkan_readback_finish(struct nx_vk_renderer *renderer) {
	if (renderer->readback.initialized) {
		release_cache(renderer->dev->dev, &renderer->readback);
	}
}
