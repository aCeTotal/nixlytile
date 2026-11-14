#include <vulkan/vulkan.h>
#include "platform/log/log.h"
#include "platform/vulkan/queue/command_buffer.h"
#include "platform/vulkan/queue/stage.h"
#include "platform/vulkan/renderer.h"

VkCommandBuffer vulkan_record_stage_cb(struct nx_vk_renderer *renderer) {
	if (renderer->stage.cb != NULL) {
		return renderer->stage.cb->vk;
	}

	renderer->stage.cb = vulkan_acquire_command_buffer(renderer);
	if (renderer->stage.cb == NULL) {
		return VK_NULL_HANDLE;
	}

	VkCommandBufferBeginInfo begin_info = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};
	vkBeginCommandBuffer(renderer->stage.cb->vk, &begin_info);
	return renderer->stage.cb->vk;
}

bool vulkan_submit_stage_wait(struct nx_vk_renderer *renderer) {
	struct nx_vk_command_buffer *cb = renderer->stage.cb;
	if (cb == NULL) {
		return false;
	}
	renderer->stage.cb = NULL;

	uint64_t timeline_point = vulkan_end_command_buffer(cb, renderer);
	if (timeline_point == 0) {
		return false;
	}

	VkTimelineSemaphoreSubmitInfoKHR timeline_submit_info = {
		.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO_KHR,
		.signalSemaphoreValueCount = 1,
		.pSignalSemaphoreValues = &timeline_point,
	};
	VkSubmitInfo submit_info = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.pNext = &timeline_submit_info,
		.commandBufferCount = 1,
		.pCommandBuffers = &cb->vk,
		.signalSemaphoreCount = 1,
		.pSignalSemaphores = &renderer->timeline_semaphore,
	};

	vulkan_stage_mark_submit(renderer, timeline_point);

	VkResult res = vkQueueSubmit(renderer->dev->queue, 1, &submit_info, VK_NULL_HANDLE);
	if (res != VK_SUCCESS) {
		nx_vk_error("vkQueueSubmit", res);
		return false;
	}
	if (!vulkan_wait_command_buffer(cb, renderer)) {
		return false;
	}

	// Blocking wait reached this point.
	vulkan_stage_buffer_gc(renderer, timeline_point);
	return true;
}
