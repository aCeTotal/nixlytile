#include <vulkan/vulkan.h>
#include "platform/log/log.h"
#include "platform/vulkan/device/device.h"

int vulkan_find_mem_type(struct nx_vk_device *dev, VkMemoryPropertyFlags flags,
		uint32_t req_bits) {
	VkPhysicalDeviceMemoryProperties props;
	vkGetPhysicalDeviceMemoryProperties(dev->phdev, &props);

	for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
		if ((req_bits & (1u << i)) &&
				(props.memoryTypes[i].propertyFlags & flags) == flags) {
			return (int)i;
		}
	}
	return -1;
}

VkDeviceMemory vulkan_alloc_image_memory(struct nx_vk_device *dev, VkImage image,
		VkMemoryPropertyFlags flags) {
	VkMemoryRequirements mem_reqs;
	vkGetImageMemoryRequirements(dev->dev, image, &mem_reqs);

	int mem_type = vulkan_find_mem_type(dev, flags, mem_reqs.memoryTypeBits);
	if (mem_type < 0) {
		nx_log(NX_ERROR, "Failed to find suitable memory type");
		return VK_NULL_HANDLE;
	}

	VkMemoryAllocateInfo mem_info = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = mem_reqs.size,
		.memoryTypeIndex = (uint32_t)mem_type,
	};
	VkDeviceMemory memory;
	VkResult res = vkAllocateMemory(dev->dev, &mem_info, NULL, &memory);
	if (res != VK_SUCCESS) {
		nx_vk_error("vkAllocateMemory failed", res);
		return VK_NULL_HANDLE;
	}

	res = vkBindImageMemory(dev->dev, image, memory, 0);
	if (res != VK_SUCCESS) {
		nx_vk_error("vkBindImageMemory failed", res);
		vkFreeMemory(dev->dev, memory, NULL);
		return VK_NULL_HANDLE;
	}
	return memory;
}
