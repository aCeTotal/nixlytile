#include "layer.h"

#include <string.h>

static _Atomic(struct device *) publisher;

static int
allocate_commands(struct device *dev)
{
	VkCommandBuffer cmds[CMD_COUNT];
	VkCommandBufferAllocateInfo info = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = dev->cmd_pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = CMD_COUNT,
	};
	uint32_t i;

	if (dev->vk.AllocateCommandBuffers(dev->handle, &info, cmds) != VK_SUCCESS)
		return 0;
	for (i = 0; i < CMD_COUNT; i++)
		dev->set_loader_data(dev->handle, cmds[i]);
	memcpy(dev->timing.begin, cmds, sizeof(dev->timing.begin));
	memcpy(dev->timing.end, cmds + TIMING_SLOTS, sizeof(dev->timing.end));
	memcpy(dev->uploads.cmd, cmds + 2 * TIMING_SLOTS, sizeof(dev->uploads.cmd));
	return 1;
}

/* Timestamped graphics queue, or none. */
static int
frame_init(struct device *dev, VkQueue queue)
{
	uint32_t family = device_family_of(dev, queue);
	VkCommandPoolCreateInfo pool = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
		.queueFamilyIndex = family,
	};
	VkQueryPoolCreateInfo queries = {
		.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
		.queryType = VK_QUERY_TYPE_TIMESTAMP,
		.queryCount = QUERY_COUNT,
	};
	uint32_t bits;

	if (family >= dev->nfamilies
			|| !(dev->families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT))
		return 0;
	bits = dev->families[family].timestampValidBits;
	if (!bits || dev->vk.CreateCommandPool(dev->handle, &pool, NULL,
			&dev->cmd_pool) != VK_SUCCESS
			|| dev->vk.CreateQueryPool(dev->handle, &queries, NULL,
				&dev->queries) != VK_SUCCESS
			|| !allocate_commands(dev) || !timing_init(dev))
		return 0;
	dev->timing.period = dev->timestamp_period;
	dev->timing.mask = bits >= 64 ? UINT64_MAX : (1ULL << bits) - 1;
	atomic_store(&dev->present, queue);
	return 1;
}

static int
usable(struct device *dev)
{
	return dev->vrs && !atomic_load(&dev->unsafe);
}

static int
claim(struct device *dev)
{
	struct device *owner = NULL;

	return atomic_compare_exchange_strong(&publisher, &owner, dev) || owner == dev;
}

static void
publish(struct device *dev, struct vrs_link *page, float budget)
{
	const struct control *c = &dev->control;
	float gpu = control_gpu(c);

	atomic_store(&page->gpu_ns, (uint32_t)gpu);
	atomic_store(&page->gpu_min_ns, (uint32_t)(usable(dev)
		? control_predict(c, VRS_LEVELS - 1) : gpu));
	atomic_store(&page->level, (uint32_t)c->level);
	atomic_store(&page->level_max, usable(dev) ? VRS_LEVELS - 1 : 0);
	atomic_store(&page->saturated, c->level == VRS_LEVELS - 1 && gpu > budget);
	atomic_fetch_add(&page->frames, 1);
}

/* nixlytile budget sets level. */
static void
steer(struct device *dev, uint64_t now)
{
	struct vrs_link *page = link_page();
	float budget = page && usable(dev) && atomic_load(&page->active)
		? (float)atomic_load(&page->budget_ns) : 0.0f;

	atomic_store(&dev->level, control_step(&dev->control, budget, now));
	if (page && claim(dev))
		publish(dev, page, budget);
}

static void
choose_present(struct device *dev, VkQueue queue)
{
	pthread_mutex_lock(&dev->lock);
	if (!atomic_load(&dev->present) && !atomic_load(&dev->present_failed))
		atomic_store(&dev->present_failed, !frame_init(dev, queue));
	pthread_mutex_unlock(&dev->lock);
}

static void
frame_end(struct device *dev, VkQueue queue)
{
	uint64_t now = now_ns();

	if (!atomic_load(&dev->present) && !atomic_load(&dev->present_failed))
		choose_present(dev, queue);
	if (queue != atomic_load(&dev->present))
		return;
	timing_harvest(dev, now);
	steer(dev, now);
	dev->frames++;
}

VKAPI_ATTR VkResult VKAPI_CALL
frame_present(VkQueue queue, const VkPresentInfoKHR *info)
{
	struct device *dev = device_of(queue);

	frame_end(dev, queue);
	return dev->vk.QueuePresentKHR(queue, info);
}

VKAPI_ATTR VkResult VKAPI_CALL
frame_swapchain(VkDevice device, const VkSwapchainCreateInfoKHR *info,
	const VkAllocationCallbacks *alloc, VkSwapchainKHR *out)
{
	struct device *dev = device_of(device);
	VkResult res = dev->vk.CreateSwapchainKHR(device, info, alloc, out);

	if (res != VK_SUCCESS)
		return res;
	atomic_store(&dev->swap_extent,
		(uint64_t)info->imageExtent.width << 32 | info->imageExtent.height);
	link_connect();
	return res;
}

void
frame_destroy(struct device *dev)
{
	struct device *self = dev;

	atomic_compare_exchange_strong(&publisher, &self, NULL);
	timing_destroy(dev);
	dev->vk.DestroyQueryPool(dev->handle, dev->queries, NULL);
	dev->vk.DestroyCommandPool(dev->handle, dev->cmd_pool, NULL);
}

static void
remember(struct device *dev, VkSemaphore semaphore)
{
	if (!semaphore)
		return;
	pthread_mutex_lock(&dev->lock);
	dev->acquired[dev->nacquired++ % ACQUIRED_MAX] = semaphore;
	pthread_mutex_unlock(&dev->lock);
}

/* Acquire semaphore: display-signaled. */
int
frame_acquired(struct device *dev, VkSemaphore semaphore)
{
	int i, found = 0;

	if (!semaphore)
		return 0;
	pthread_mutex_lock(&dev->lock);
	for (i = 0; i < ACQUIRED_MAX && !found; i++)
		found = dev->acquired[i] == semaphore;
	pthread_mutex_unlock(&dev->lock);
	return found;
}

VKAPI_ATTR VkResult VKAPI_CALL
frame_acquire(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout,
	VkSemaphore semaphore, VkFence fence, uint32_t *index)
{
	struct device *dev = device_of(device);
	VkResult res = dev->vk.AcquireNextImageKHR(device, swapchain, timeout,
		semaphore, fence, index);

	if (res == VK_SUCCESS || res == VK_SUBOPTIMAL_KHR)
		remember(dev, semaphore);
	return res;
}

VKAPI_ATTR VkResult VKAPI_CALL
frame_acquire2(VkDevice device, const VkAcquireNextImageInfoKHR *info,
	uint32_t *index)
{
	struct device *dev = device_of(device);
	VkResult res = dev->vk.AcquireNextImage2KHR(device, info, index);

	if (res == VK_SUCCESS || res == VK_SUBOPTIMAL_KHR)
		remember(dev, info->semaphore);
	return res;
}
