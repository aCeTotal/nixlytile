#include "layer.h"

/* Display size for scenes, maps. */
VKAPI_ATTR VkResult VKAPI_CALL
swap_create(VkDevice device, const VkSwapchainCreateInfoKHR *create,
	const VkAllocationCallbacks *alloc, VkSwapchainKHR *out)
{
	struct device *dev = device_of(device);
	VkResult created = dev->vk.CreateSwapchainKHR(device, create, alloc, out);

	if (created != VK_SUCCESS)
		return created;
	atomic_store(&dev->swap_extent, extent_key(create->imageExtent));
	link_connect();
	return created;
}
