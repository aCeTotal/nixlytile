#include "layer.h"

#include <string.h>

#define SUBMIT_MAX 16
#define WAITS_MAX  16

struct build {
	VkSubmitInfo batches[SUBMIT_MAX];
	VkCommandBuffer pre[2];
	VkSemaphore acquire[WAITS_MAX];
	VkPipelineStageFlags acquire_stages[WAITS_MAX];
	VkSemaphore waits[WAITS_MAX];
	VkPipelineStageFlags wait_stages[WAITS_MAX];
	VkSemaphore hop;
	VkCommandBuffer end;
};

struct build2 {
	VkSubmitInfo2 batches[SUBMIT_MAX];
	VkCommandBufferSubmitInfo pre[2];
	VkSemaphoreSubmitInfo acquire[WAITS_MAX];
	VkSemaphoreSubmitInfo waits[WAITS_MAX];
	VkSemaphoreSubmitInfo hop;
	VkCommandBufferSubmitInfo end;
};

/* Present queue only. */
static int
prepare(struct device *dev, VkQueue queue, struct extras *x)
{
	if (queue != atomic_load(&dev->present))
		return 0;
	timing_harvest(dev, now_ns());
	rate_settle(dev);
	x->upload = upload_changes(dev);
	timing_claim(dev, x);
	return x->upload || x->begin;
}

static int
plain_batch(const VkSubmitInfo *s)
{
	const VkProtectedSubmitInfo *protect = (const VkProtectedSubmitInfo *)
		chain_find(s->pNext, VK_STRUCTURE_TYPE_PROTECTED_SUBMIT_INFO);

	return !chain_find(s->pNext, VK_STRUCTURE_TYPE_DEVICE_GROUP_SUBMIT_INFO)
		&& !(protect && protect->protectedSubmit)
		&& s->waitSemaphoreCount < WAITS_MAX;
}

/* Acquire waits precede start stamp. */
static void
interpose(struct device *dev, struct build *b)
{
	VkSubmitInfo *pre = &b->batches[0], *first = &b->batches[1];
	VkPipelineStageFlags mask = 0;
	uint32_t i, nacquire = 0, nwait = 0;

	if (chain_find(first->pNext, VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO))
		return;
	for (i = 0; i < first->waitSemaphoreCount; i++) {
		VkSemaphore s = first->pWaitSemaphores[i];
		if (!frame_acquired(dev, s)) {
			b->wait_stages[nwait] = first->pWaitDstStageMask[i];
			b->waits[nwait++] = s;
			continue;
		}
		mask |= first->pWaitDstStageMask[i];
		b->acquire_stages[nacquire] = VK_PIPELINE_STAGE_TRANSFER_BIT;
		b->acquire[nacquire++] = s;
	}
	if (!nacquire)
		return;
	b->wait_stages[nwait] = mask;
	b->waits[nwait++] = b->hop;
	pre->waitSemaphoreCount = nacquire;
	pre->pWaitSemaphores = b->acquire;
	pre->pWaitDstStageMask = b->acquire_stages;
	pre->signalSemaphoreCount = 1;
	pre->pSignalSemaphores = &b->hop;
	first->waitSemaphoreCount = nwait;
	first->pWaitSemaphores = b->waits;
	first->pWaitDstStageMask = b->wait_stages;
}

/* Upload, start, app batches, end. */
VKAPI_ATTR VkResult VKAPI_CALL
submit_queue(VkQueue queue, uint32_t count, const VkSubmitInfo *submits,
	VkFence fence)
{
	struct device *dev = device_of(queue);
	struct extras x = {0};
	struct build b;
	uint32_t pre = 0, total = count + 1;
	VkResult res;

	if (!count || count + 2 > SUBMIT_MAX || !plain_batch(&submits[0])
			|| !prepare(dev, queue, &x))
		return dev->vk.QueueSubmit(queue, count, submits, fence);
	b.pre[pre] = x.upload;
	pre += x.upload != VK_NULL_HANDLE;
	b.pre[pre] = x.begin;
	pre += x.begin != VK_NULL_HANDLE;
	b.batches[0] = (VkSubmitInfo){ .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.commandBufferCount = pre, .pCommandBuffers = b.pre };
	memcpy(&b.batches[1], submits, count * sizeof(*submits));
	b.hop = x.hop;
	b.end = x.end;
	if (x.begin)
		interpose(dev, &b);
	if (x.end)
		b.batches[total++] = (VkSubmitInfo){ .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
			.commandBufferCount = 1, .pCommandBuffers = &b.end };
	res = dev->vk.QueueSubmit(queue, total, b.batches, fence);
	if (res != VK_SUCCESS)
		timing_abandon(dev, &x);
	return res;
}

static VkCommandBufferSubmitInfo
cmd_info(VkCommandBuffer cmd)
{
	return (VkCommandBufferSubmitInfo){
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
		.commandBuffer = cmd,
	};
}

static void
interpose2(struct device *dev, struct build2 *b)
{
	VkSubmitInfo2 *pre = &b->batches[0], *first = &b->batches[1];
	VkPipelineStageFlags2 mask = 0;
	uint32_t i, nacquire = 0, nwait = 0;

	for (i = 0; i < first->waitSemaphoreInfoCount; i++) {
		const VkSemaphoreSubmitInfo *w = &first->pWaitSemaphoreInfos[i];
		if (!frame_acquired(dev, w->semaphore)) {
			b->waits[nwait++] = *w;
			continue;
		}
		mask |= w->stageMask;
		b->acquire[nacquire] = *w;
		b->acquire[nacquire++].stageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
	}
	if (!nacquire)
		return;
	b->waits[nwait] = b->hop;
	b->waits[nwait++].stageMask = mask;
	pre->waitSemaphoreInfoCount = nacquire;
	pre->pWaitSemaphoreInfos = b->acquire;
	pre->signalSemaphoreInfoCount = 1;
	pre->pSignalSemaphoreInfos = &b->hop;
	first->waitSemaphoreInfoCount = nwait;
	first->pWaitSemaphoreInfos = b->waits;
}

VKAPI_ATTR VkResult VKAPI_CALL
submit_queue2(VkQueue queue, uint32_t count, const VkSubmitInfo2 *submits,
	VkFence fence)
{
	struct device *dev = device_of(queue);
	struct extras x = {0};
	struct build2 b;
	uint32_t pre = 0, total = count + 1;
	VkResult res;

	if (!count || count + 2 > SUBMIT_MAX
			|| (submits[0].flags & VK_SUBMIT_PROTECTED_BIT)
			|| submits[0].waitSemaphoreInfoCount >= WAITS_MAX
			|| !prepare(dev, queue, &x))
		return dev->vk.QueueSubmit2(queue, count, submits, fence);
	b.pre[pre] = cmd_info(x.upload);
	pre += x.upload != VK_NULL_HANDLE;
	b.pre[pre] = cmd_info(x.begin);
	pre += x.begin != VK_NULL_HANDLE;
	b.batches[0] = (VkSubmitInfo2){ .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
		.commandBufferInfoCount = pre, .pCommandBufferInfos = b.pre };
	memcpy(&b.batches[1], submits, count * sizeof(*submits));
	b.hop = (VkSemaphoreSubmitInfo){ .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
		.semaphore = x.hop, .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT };
	b.end = cmd_info(x.end);
	if (x.begin)
		interpose2(dev, &b);
	if (x.end)
		b.batches[total++] = (VkSubmitInfo2){ .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
			.commandBufferInfoCount = 1, .pCommandBufferInfos = &b.end };
	res = dev->vk.QueueSubmit2(queue, total, b.batches, fence);
	if (res != VK_SUCCESS)
		timing_abandon(dev, &x);
	return res;
}
