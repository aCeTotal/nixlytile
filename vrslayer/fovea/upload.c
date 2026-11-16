#include "layer.h"

#include <stdio.h>

#define RATE_STAGE VK_PIPELINE_STAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR

/* Maps one copy carries. */
struct pending {
	struct map map;
	struct rate_image *rates[RATE_SIZES];
	uint32_t count;
	uint32_t slot;
};

static int
slot_init(struct device *dev, struct upload_slot *s, VkCommandBuffer cmd)
{
	VkFenceCreateInfo fence = {
		.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
		.flags = VK_FENCE_CREATE_SIGNALED_BIT,
	};

	dev->set_loader_data(dev->handle, cmd);
	s->cmd = cmd;
	return dev->vk.CreateFence(dev->handle, &fence, NULL, &s->fence) == VK_SUCCESS;
}

static int
slots_create(struct device *dev)
{
	struct fovea *f = &dev->fovea;
	VkCommandPoolCreateInfo pool = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
		.queueFamilyIndex = f->family,
	};
	VkCommandBufferAllocateInfo allocate = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = UPLOAD_SLOTS,
	};
	VkCommandBuffer cmds[UPLOAD_SLOTS];
	int i = 0;

	if (dev->vk.CreateCommandPool(dev->handle, &pool, NULL, &f->pool) != VK_SUCCESS)
		return 0;
	allocate.commandPool = f->pool;
	if (dev->vk.AllocateCommandBuffers(dev->handle, &allocate, cmds) != VK_SUCCESS)
		return 0;
	while (i < UPLOAD_SLOTS && slot_init(dev, &f->slots[i], cmds[i]))
		i++;
	return i == UPLOAD_SLOTS;
}

/* Present thread; worker creates. */
void
upload_request(struct device *dev, uint32_t family)
{
	dev->fovea.family = family;
	atomic_store(&dev->fovea.upload, UPLOAD_WANTED);
	worker_kick(&dev->fovea.worker);
}

/* Worker, once family known. */
void
upload_create(struct device *dev)
{
	struct fovea *f = &dev->fovea;

	if (atomic_load(&f->upload) != UPLOAD_WANTED)
		return;
	if (!slots_create(dev)) {
		atomic_store(&f->upload, UPLOAD_FAILED);
		fprintf(stderr, "nixly-vrs: no upload objects, rates native\n");
		return;
	}
	atomic_store(&f->upload, UPLOAD_READY);
}

/* Device idle, worker joined. */
void
upload_destroy(struct device *dev)
{
	struct fovea *f = &dev->fovea;
	int i;

	for (i = 0; i < UPLOAD_SLOTS; i++)
		dev->vk.DestroyFence(dev->handle, f->slots[i].fence, NULL);
	dev->vk.DestroyCommandPool(dev->handle, f->pool, NULL);
}

/* New, or zones since changed. */
static int
stale(const struct rate_image *r, const struct zones *now)
{
	enum rate_state state = atomic_load(&r->state);

	return state == RATE_FRESH || (state == RATE_READY && !zones_equal(&r->zones, now));
}

static void
take(struct pending *p, struct rate_image *r, const struct zones *now)
{
	if (stale(r, now))
		p->rates[p->count++] = r;
}

static struct pending
gather(struct device *dev)
{
	struct fovea *f = &dev->fovea;
	struct pending p = {
		.map = { f->zones, dev->fit, dev->texel },
		.slot = f->next % UPLOAD_SLOTS,
	};
	int i;

	for (i = 0; i < RATE_SIZES; i++)
		take(&p, &f->rates[i], &f->zones);
	return p;
}

/* Slot's share of image staging. */
static VkDeviceSize
region(const struct rate_image *r, uint32_t slot)
{
	return slot * grid_bytes(r->grid);
}

static void
fill(const struct pending *p)
{
	const struct rate_image *r;
	uint32_t i;

	for (i = 0; i < p->count; i++) {
		r = p->rates[i];
		map_fill(&p->map, r->size, r->mapped + region(r, p->slot));
	}
}

/* Later frames read after copy. */
static void
record(struct device *dev, const struct upload_slot *s, const struct pending *p)
{
	VkImageMemoryBarrier images[RATE_SIZES];
	VkMemoryBarrier readable = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_FRAGMENT_SHADING_RATE_ATTACHMENT_READ_BIT_KHR,
	};
	VkBufferImageCopy copy = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 } };
	const struct rate_image *r;
	uint32_t i;

	for (i = 0; i < p->count; i++)
		images[i] = rate_writable(p->rates[i]->attachment.image);
	dev->vk.CmdPipelineBarrier(s->cmd, RATE_STAGE, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
		0, NULL, 0, NULL, p->count, images);
	for (i = 0; i < p->count; i++) {
		r = p->rates[i];
		copy.bufferOffset = region(r, p->slot);
		copy.imageExtent = (VkExtent3D){ r->grid.width, r->grid.height, 1 };
		dev->vk.CmdCopyBufferToImage(s->cmd, r->staging.buffer, r->attachment.image,
			VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
	}
	dev->vk.CmdPipelineBarrier(s->cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, RATE_STAGE, 0,
		1, &readable, 0, NULL, 0, NULL);
}

/* No waits: never touches swapchain. */
static int
queue_copy(struct device *dev, const struct upload_slot *s)
{
	VkSubmitInfo submit = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.commandBufferCount = 1,
		.pCommandBuffers = &s->cmd,
	};

	return dev->vk.ResetFences(dev->handle, 1, &s->fence) == VK_SUCCESS
		&& dev->vk.QueueSubmit(atomic_load(&dev->present), 1, &submit, s->fence)
			== VK_SUCCESS;
}

static int
upload(struct device *dev, const struct pending *p)
{
	const struct upload_slot *s = &dev->fovea.slots[p->slot];
	VkCommandBufferBeginInfo begin = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};

	if (dev->vk.BeginCommandBuffer(s->cmd, &begin) != VK_SUCCESS)
		return 0;
	fill(p);
	record(dev, s, p);
	return dev->vk.EndCommandBuffer(s->cmd) == VK_SUCCESS && queue_copy(dev, s);
}

static void
commit(const struct pending *p)
{
	struct rate_image *r;
	uint32_t i;

	for (i = 0; i < p->count; i++) {
		r = p->rates[i];
		r->zones = p->map.zones;
		atomic_store(&r->native, map_native(&p->map, r->size));
		atomic_store(&r->state, RATE_READY);
	}
}

/* Present thread; 0: retry later. */
int
upload_stale(struct device *dev)
{
	struct fovea *f = &dev->fovea;
	struct pending p = gather(dev);

	if (!p.count)
		return 1;
	if (dev->vk.GetFenceStatus(dev->handle, f->slots[p.slot].fence) != VK_SUCCESS)
		return 0;
	if (!upload(dev, &p)) {
		atomic_store(&f->upload, UPLOAD_FAILED);
		fprintf(stderr, "nixly-vrs: rate upload failed, rates native\n");
		return 1;
	}
	commit(&p);
	f->next++;
	return 1;
}
