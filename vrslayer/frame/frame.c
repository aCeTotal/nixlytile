#include "layer.h"

#define LINK_CHECK_FRAMES 120

/* Graphics queue, uploads rates. */
static int
frame_init(struct device *dev, VkQueue queue)
{
	uint32_t family = device_family_of(dev, queue);

	if (family >= dev->nfamilies
			|| !(dev->families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT))
		return 0;
	upload_request(dev, family);
	atomic_store(&dev->present, queue);
	return 1;
}

static void
choose_present(struct device *dev, VkQueue queue)
{
	pthread_mutex_lock(&dev->lock);
	if (!atomic_load(&dev->present) && !atomic_load(&dev->present_failed))
		atomic_store(&dev->present_failed, !frame_init(dev, queue));
	pthread_mutex_unlock(&dev->lock);
}

/* Copies precede the present. */
VKAPI_ATTR VkResult VKAPI_CALL
frame_present(VkQueue queue, const VkPresentInfoKHR *present)
{
	struct device *dev = device_of(queue);

	if (!atomic_load(&dev->present) && !atomic_load(&dev->present_failed))
		choose_present(dev, queue);
	if (queue != atomic_load(&dev->present))
		return dev->vk.QueuePresentKHR(queue, present);
	if (atomic_fetch_add(&dev->frames, 1) % LINK_CHECK_FRAMES == 0)
		link_check();
	fovea_present(dev);
	return dev->vk.QueuePresentKHR(queue, present);
}
