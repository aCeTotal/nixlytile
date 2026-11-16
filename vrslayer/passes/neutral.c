#include "passes.h"

#define CLEAR_QUEUE (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)
#define NO_FAMILY   UINT32_MAX

/* Families the app renders on. */
struct families {
	uint32_t index[FAMILIES_MAX];
	uint32_t count;
	uint32_t clear;             /* family used to clear */
};

static int
listed(const struct families *fam, uint32_t family)
{
	uint32_t i = 0;

	while (i < fam->count && fam->index[i] != family)
		i++;
	return i < fam->count;
}

/* Lists family; picks clear family. */
static void
add_queue(const struct device *dev, struct families *fam,
	const VkDeviceQueueCreateInfo *q)
{
	if (!listed(fam, q->queueFamilyIndex))
		fam->index[fam->count++] = q->queueFamilyIndex;
	if (fam->clear == NO_FAMILY && !q->flags && q->queueFamilyIndex < dev->nfamilies
			&& dev->families[q->queueFamilyIndex].queueFlags & CLEAR_QUEUE)
		fam->clear = q->queueFamilyIndex;
}

static void
pick_families(const struct device *dev, const VkDeviceCreateInfo *requested,
	struct families *fam)
{
	uint32_t i;

	fam->count = 0;
	fam->clear = NO_FAMILY;
	for (i = 0; i < requested->queueCreateInfoCount && fam->count < FAMILIES_MAX; i++)
		add_queue(dev, fam, &requested->pQueueCreateInfos[i]);
}

static struct gpu_image
create_image(struct device *dev, const struct neutral *n, const struct families *fam)
{
	VkImageCreateInfo create = rate_image_create(n->grid);

	if (fam->count > 1) {
		create.sharingMode = VK_SHARING_MODE_CONCURRENT;
		create.queueFamilyIndexCount = fam->count;
		create.pQueueFamilyIndices = fam->index;
	}
	return gpu_image(dev, &create);
}

static void
record_clear(struct device *dev, VkCommandBuffer cmd, VkImage image)
{
	static const VkClearColorValue native = { .uint32 = { 0 } };
	VkImageMemoryBarrier b = rate_writable(image);

	dev->vk.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
	dev->vk.CmdClearColorImage(cmd, image, VK_IMAGE_LAYOUT_GENERAL, &native, 1,
		&WHOLE_COLOR);
	b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	b.dstAccessMask = VK_ACCESS_FRAGMENT_SHADING_RATE_ATTACHMENT_READ_BIT_KHR;
	b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
	dev->vk.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR, 0, 0, NULL,
		0, NULL, 1, &b);
}

/* End, submit, wait on cmd. */
static int
finish(struct device *dev, VkQueue queue, VkCommandBuffer cmd)
{
	VkFenceCreateInfo fence_create = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	VkSubmitInfo submit = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.commandBufferCount = 1,
		.pCommandBuffers = &cmd,
	};
	VkFence fence = VK_NULL_HANDLE;
	int done;

	done = dev->vk.EndCommandBuffer(cmd) == VK_SUCCESS
		&& dev->vk.CreateFence(dev->handle, &fence_create, NULL, &fence) == VK_SUCCESS
		&& dev->vk.QueueSubmit(queue, 1, &submit, fence) == VK_SUCCESS
		&& dev->vk.WaitForFences(dev->handle, 1, &fence, VK_TRUE, UINT64_MAX)
			== VK_SUCCESS;
	dev->vk.DestroyFence(dev->handle, fence, NULL);
	return done;
}

/* Inside vkCreateDevice: queue idle. */
static int
submit_clear(struct device *dev, VkCommandPool pool, uint32_t family)
{
	VkCommandBufferAllocateInfo alloc = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	};
	VkCommandBufferBeginInfo begin = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};
	VkCommandBuffer cmd;
	VkQueue queue;

	dev->vk.GetDeviceQueue(dev->handle, family, 0, &queue);
	if (!queue || dev->vk.AllocateCommandBuffers(dev->handle, &alloc, &cmd) != VK_SUCCESS)
		return 0;
	dev->set_loader_data(dev->handle, queue);
	dev->set_loader_data(dev->handle, cmd);
	dev->vk.BeginCommandBuffer(cmd, &begin);
	record_clear(dev, cmd, dev->passes->neutral.image.image);
	return finish(dev, queue, cmd);
}

static int
clear(struct device *dev, uint32_t family)
{
	VkCommandPoolCreateInfo create = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
		.queueFamilyIndex = family,
	};
	VkCommandPool pool;
	int done;

	if (dev->vk.CreateCommandPool(dev->handle, &create, NULL, &pool) != VK_SUCCESS)
		return 0;
	done = submit_clear(dev, pool, family);
	dev->vk.DestroyCommandPool(dev->handle, pool, NULL);
	return done;
}

/* All-1x1 rate image, initialized. */
void
neutral_create(struct device *dev, const VkDeviceCreateInfo *requested)
{
	struct neutral *n = &dev->passes->neutral;
	struct families fam;

	pick_families(dev, requested, &fam);
	n->grid = texel_grid(dev->texel, dev->max_framebuffer);
	if (fam.clear != NO_FAMILY)
		n->image = create_image(dev, n, &fam);
	if (!n->image.view || !clear(dev, fam.clear))
		neutral_destroy(dev);
}

void
neutral_destroy(struct device *dev)
{
	struct neutral *n = &dev->passes->neutral;

	gpu_image_free(dev, &n->image);
	n->grid = (VkExtent2D){0};
}
