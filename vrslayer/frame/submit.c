#include "passes.h"

static void
watch(struct device *dev, const VkCommandBuffer *cmds, uint32_t n)
{
	uint32_t i;

	for (i = 0; i < n; i++)
		passes_check(dev, cmds[i]);
}

static void
watch2(struct device *dev, const VkCommandBufferSubmitInfo *cmds, uint32_t n)
{
	uint32_t i;

	for (i = 0; i < n; i++)
		passes_check(dev, cmds[i].commandBuffer);
}

/* Rates live on present queue. */
static int
elsewhere(struct device *dev, VkQueue queue)
{
	return !atomic_load(&dev->split) && queue != atomic_load(&dev->present);
}

VKAPI_ATTR VkResult VKAPI_CALL
submit_queue(VkQueue queue, uint32_t count, const VkSubmitInfo *submits,
	VkFence fence)
{
	struct device *dev = device_of(queue);
	uint32_t i, n = elsewhere(dev, queue) ? count : 0;

	for (i = 0; i < n; i++)
		watch(dev, submits[i].pCommandBuffers, submits[i].commandBufferCount);
	return dev->vk.QueueSubmit(queue, count, submits, fence);
}

VKAPI_ATTR VkResult VKAPI_CALL
submit_queue2(VkQueue queue, uint32_t count, const VkSubmitInfo2 *submits,
	VkFence fence)
{
	struct device *dev = device_of(queue);
	uint32_t i, n = elsewhere(dev, queue) ? count : 0;

	for (i = 0; i < n; i++)
		watch2(dev, submits[i].pCommandBufferInfos, submits[i].commandBufferInfoCount);
	return dev->vk.QueueSubmit2(queue, count, submits, fence);
}
