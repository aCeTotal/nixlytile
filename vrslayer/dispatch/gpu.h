#ifndef NIXLY_VRS_GPU_H
#define NIXLY_VRS_GPU_H

#include <vulkan/vulkan.h>

#define WHOLE_COLOR ((VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 })

/* Image, view and memory. */
struct gpu_image {
	VkImage image;
	VkImageView view;
	VkDeviceMemory memory;
};

struct gpu_buffer {
	VkBuffer buffer;
	VkDeviceMemory memory;
};

struct device;

struct gpu_buffer gpu_buffer(struct device *dev, const VkBufferCreateInfo *create,
	VkMemoryPropertyFlags flags);
void gpu_buffer_free(struct device *dev, struct gpu_buffer *b);
struct gpu_image gpu_image(struct device *dev, const VkImageCreateInfo *create);
void gpu_image_free(struct device *dev, struct gpu_image *g);

#endif
