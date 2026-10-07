#include "layer.h"

#define CURSOR_SCALE 0.4f

struct paint_params {
	int level;
	uint32_t cursor;
};

struct upload_job {
	struct rate_image *rates[RATE_SIZES];
	int count;
	uint32_t slot;
	struct paint_params params;
};

struct recording {
	struct device *dev;
	VkCommandBuffer cmd;
	const struct upload_job *job;
};

static int
slot_done(struct device *dev, uint32_t slot)
{
	return !dev->uploads.used[slot]
		|| timing_passed(dev, dev->uploads.mark[slot]);
}

static struct paint_params
current_params(struct device *dev)
{
	struct vrs_link *page = link_page();
	int level = atomic_load(&dev->level);

	return (struct paint_params){
		.level = level,
		.cursor = page && level ? atomic_load(&page->cursor) : VRS_CURSOR_NONE,
	};
}

/* Level plus cursor tile. */
static uint64_t
paint_key(const struct rate_image *r, struct paint_params p)
{
	uint64_t x, y;

	if (p.cursor == VRS_CURSOR_NONE)
		return (uint64_t)p.level << 48 | 1ULL << 47;
	x = (uint64_t)(p.cursor >> 16) * r->grid.width / VRS_CURSOR_SCALE;
	y = (uint64_t)(p.cursor & 0xffffu) * r->grid.height / VRS_CURSOR_SCALE;
	return (uint64_t)p.level << 48 | x << 16 | y;
}

static struct pattern
pattern_for(const struct device *dev, const struct rate_image *r,
	struct paint_params params)
{
	struct pattern p = {
		.codes = dev->codes,
		.level = params.level,
		.foci = 1,
		.focus[0] = { 0.5f * (float)r->size.width, 0.5f * (float)r->size.height, 1.0f },
	};

	if (params.cursor == VRS_CURSOR_NONE)
		return p;
	p.focus[1] = (struct pattern_focus){
		(float)(params.cursor >> 16) / VRS_CURSOR_SCALE * (float)r->size.width,
		(float)(params.cursor & 0xffffu) / VRS_CURSOR_SCALE * (float)r->size.height,
		CURSOR_SCALE,
	};
	p.foci = 2;
	return p;
}

static void
barrier_before_copy(const struct recording *rec, struct rate_image *r)
{
	VkImageMemoryBarrier b = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		.oldLayout = r->initialized ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED,
		.newLayout = VK_IMAGE_LAYOUT_GENERAL,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = r->image,
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
	};

	rec->dev->vk.CmdPipelineBarrier(rec->cmd,
		VK_PIPELINE_STAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR,
		VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}

static void
barrier_after_copy(const struct recording *rec, struct rate_image *r)
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
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
	};

	rec->dev->vk.CmdPipelineBarrier(rec->cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR,
		0, 0, NULL, 0, NULL, 1, &b);
}

static void
upload_one(const struct recording *rec, struct rate_image *r)
{
	const struct upload_job *job = rec->job;
	struct pattern p = pattern_for(rec->dev, r, job->params);
	VkDeviceSize offset = job->slot * r->slot_bytes;
	struct pattern_grid g = {
		.texels = r->mapped + offset,
		.columns = r->grid.width,
		.rows = r->grid.height,
		.texel_w = rec->dev->texel.width,
		.texel_h = rec->dev->texel.height,
		.width = r->size.width,
		.height = r->size.height,
	};
	VkBufferImageCopy copy = {
		.bufferOffset = offset,
		.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
		.imageExtent = { r->grid.width, r->grid.height, 1 },
	};

	pattern_paint(&g, &p);
	barrier_before_copy(rec, r);
	rec->dev->vk.CmdCopyBufferToImage(rec->cmd, r->staging, r->image,
		VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
	barrier_after_copy(rec, r);
	r->painted = paint_key(r, job->params);
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
needs_paint(struct rate_image *r, struct paint_params params)
{
	int state = atomic_load(&r->state);

	return state == RATE_FRESH
		|| (state == RATE_READY && r->painted != paint_key(r, params));
}

/* Changed images only; else NULL. */
VkCommandBuffer
upload_changes(struct device *dev)
{
	struct upload_job job = {
		.slot = dev->uploads.seq % UPLOAD_SLOTS,
		.params = current_params(dev),
	};
	int i;

	if (!dev->vrs || !slot_done(dev, job.slot))
		return VK_NULL_HANDLE;
	for (i = 0; i < RATE_SIZES; i++)
		if (needs_paint(&dev->rates[i], job.params))
			job.rates[job.count++] = &dev->rates[i];
	return job.count ? record_upload(dev, &job) : VK_NULL_HANDLE;
}
