#include "layer.h"

struct upload_job {
	struct rate_image *rates[RATE_SIZES];
	int count;
	uint32_t slot;
	int level;
};

struct recording {
	struct device *dev;
	VkCommandBuffer cmd;
	const struct upload_job *job;
};

static const VkImageSubresourceRange whole = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

static int
slot_done(struct device *dev, uint32_t slot)
{
	return !dev->uploads.used[slot]
		|| timing_passed(dev, dev->uploads.mark[slot]);
}

static void
barrier_before_clear(const struct recording *rec, struct rate_image *r)
{
	VkImageMemoryBarrier b = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		.oldLayout = r->initialized ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED,
		.newLayout = VK_IMAGE_LAYOUT_GENERAL,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = r->image,
		.subresourceRange = whole,
	};

	rec->dev->vk.CmdPipelineBarrier(rec->cmd,
		VK_PIPELINE_STAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR,
		VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}

static void
barrier_after_clear(const struct recording *rec, struct rate_image *r)
{
	VkImageMemoryBarrier b = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_FRAGMENT_SHADING_RATE_ATTACHMENT_READ_BIT_KHR,
		.oldLayout = VK_IMAGE_LAYOUT_GENERAL,
		.newLayout = VK_IMAGE_LAYOUT_GENERAL,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = r->image,
		.subresourceRange = whole,
	};

	rec->dev->vk.CmdPipelineBarrier(rec->cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR,
		0, 0, NULL, 0, NULL, 1, &b);
}

/* One rate fills the screen. */
static void
upload_one(const struct recording *rec, struct rate_image *r)
{
	int level = rec->job->level;
	VkClearColorValue rate = { .uint32 = { rec->dev->codes[level] } };

	barrier_before_clear(rec, r);
	rec->dev->vk.CmdClearColorImage(rec->cmd, r->image, VK_IMAGE_LAYOUT_GENERAL,
		&rate, 1, &whole);
	barrier_after_clear(rec, r);
	r->painted = level;
	r->initialized = 1;
	if (atomic_load(&r->state) != RATE_FRESH)
		return;
	r->upload_mark = timing_mark(rec->dev);
	atomic_store(&r->state, RATE_UPLOADING);
}

static VkCommandBuffer
record_upload(struct device *dev, const struct upload_job *job)
{
	VkCommandBufferBeginInfo begin = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};
	struct recording rec = { dev, dev->uploads.cmd[job->slot], job };
	int i;

	if (dev->vk.BeginCommandBuffer(rec.cmd, &begin) != VK_SUCCESS)
		return VK_NULL_HANDLE;
	for (i = 0; i < job->count; i++)
		upload_one(&rec, job->rates[i]);
	if (dev->vk.EndCommandBuffer(rec.cmd) != VK_SUCCESS)
		return VK_NULL_HANDLE;
	dev->uploads.used[job->slot] = 1;
	dev->uploads.mark[job->slot] = timing_mark(dev);
	dev->uploads.seq++;
	return rec.cmd;
}

static int
needs_paint(struct rate_image *r, int level)
{
	int state = atomic_load(&r->state);

	return state == RATE_FRESH || (state == RATE_READY && r->painted != level);
}

/* Changed images only; else NULL. */
VkCommandBuffer
upload_changes(struct device *dev)
{
	struct upload_job job = {
		.slot = dev->uploads.seq % UPLOAD_SLOTS,
		.level = atomic_load(&dev->level),
	};
	int i;

	if (!dev->vrs || !slot_done(dev, job.slot))
		return VK_NULL_HANDLE;
	for (i = 0; i < RATE_SIZES; i++)
		if (needs_paint(&dev->rates[i], job.level))
			job.rates[job.count++] = &dev->rates[i];
	return job.count ? record_upload(dev, &job) : VK_NULL_HANDLE;
}
