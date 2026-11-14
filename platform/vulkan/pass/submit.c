#include <vulkan/vulkan.h>
#include "platform/buffer/buffer.h"
#include "platform/log/log.h"
#include "platform/vulkan/image/render_buffer.h"
#include "platform/vulkan/pass/pass.h"
#include "platform/vulkan/queue/command_buffer.h"
#include "platform/vulkan/queue/stage.h"
#include "platform/vulkan/renderer.h"

static struct nx_vk_command_buffer *take_stage_cb(struct nx_vk_renderer *renderer) {
	if (vulkan_record_stage_cb(renderer) == VK_NULL_HANDLE) {
		return NULL;
	}
	struct nx_vk_command_buffer *stage_cb = renderer->stage.cb;
	renderer->stage.cb = NULL;
	return stage_cb;
}

// Exportable sync_file semaphore.
static bool ensure_binary_semaphore(struct nx_vk_renderer *renderer,
		struct nx_vk_command_buffer *cb) {
	if (cb->binary_semaphore != VK_NULL_HANDLE) {
		return true;
	}

	VkExportSemaphoreCreateInfo export_info = {
		.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
		.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
	};
	VkSemaphoreCreateInfo semaphore_info = {
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
		.pNext = &export_info,
	};
	VkResult res = vkCreateSemaphore(renderer->dev->dev, &semaphore_info, NULL,
		&cb->binary_semaphore);
	if (res != VK_SUCCESS) {
		nx_vk_error("vkCreateSemaphore", res);
		return false;
	}
	return true;
}

static VkSemaphoreSubmitInfoKHR timeline_signal(struct nx_vk_renderer *renderer,
		uint64_t point) {
	return (VkSemaphoreSubmitInfoKHR){
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO_KHR,
		.semaphore = renderer->timeline_semaphore,
		.value = point,
	};
}

// Signal count, 0 on failure.
static uint32_t render_signals(struct nx_vk_render_pass *pass, uint64_t point,
		VkSemaphoreSubmitInfoKHR signals[static 2]) {
	struct nx_vk_renderer *renderer = pass->renderer;
	signals[0] = timeline_signal(renderer, point);
	if (!renderer->dev->implicit_sync_interop && pass->signal_timeline == NULL) {
		return 1;
	}
	if (!ensure_binary_semaphore(renderer, pass->command_buffer)) {
		return 0;
	}
	signals[1] = (VkSemaphoreSubmitInfoKHR){
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO_KHR,
		.semaphore = pass->command_buffer->binary_semaphore,
	};
	return 2;
}

// One queue orders stage first.
static VkResult queue_submit(struct nx_vk_render_pass *pass,
		struct nx_vk_command_buffer *stage_cb, const struct nx_vk_wait_list *render_waits) {
	struct nx_vk_renderer *renderer = pass->renderer;
	struct nx_vk_command_buffer *render_cb = pass->command_buffer;

	uint64_t stage_point = vulkan_end_command_buffer(stage_cb, renderer);
	if (stage_point == 0) {
		return VK_ERROR_UNKNOWN;
	}
	VkSemaphoreSubmitInfoKHR stage_wait = {
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO_KHR,
		.semaphore = renderer->timeline_semaphore,
		.value = renderer->stage.last_timeline_point,
		.stageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT_KHR,
	};
	bool wait_previous_stage = renderer->stage.last_timeline_point > 0;
	renderer->stage.last_timeline_point = stage_point;

	uint64_t render_point = vulkan_end_command_buffer(render_cb, renderer);
	if (render_point == 0) {
		return VK_ERROR_UNKNOWN;
	}
	// Deferred destruction waits here.
	pass->render_buffer->last_timeline_point = render_point;

	VkSemaphoreSubmitInfoKHR render_signal[2];
	uint32_t render_signal_len = render_signals(pass, render_point, render_signal);
	if (render_signal_len == 0) {
		return VK_ERROR_UNKNOWN;
	}

	VkSemaphoreSubmitInfoKHR stage_signal = timeline_signal(renderer, stage_point);
	VkCommandBufferSubmitInfoKHR stage_cb_info = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO_KHR,
		.commandBuffer = stage_cb->vk,
	};
	VkCommandBufferSubmitInfoKHR render_cb_info = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO_KHR,
		.commandBuffer = render_cb->vk,
	};
	VkSubmitInfo2KHR submit_info[] = {
		{
			.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2_KHR,
			.waitSemaphoreInfoCount = wait_previous_stage ? 1 : 0,
			.pWaitSemaphoreInfos = wait_previous_stage ? &stage_wait : NULL,
			.commandBufferInfoCount = 1,
			.pCommandBufferInfos = &stage_cb_info,
			.signalSemaphoreInfoCount = 1,
			.pSignalSemaphoreInfos = &stage_signal,
		},
		{
			.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2_KHR,
			.waitSemaphoreInfoCount = render_waits->len,
			.pWaitSemaphoreInfos = render_waits->infos,
			.commandBufferInfoCount = 1,
			.pCommandBufferInfos = &render_cb_info,
			.signalSemaphoreInfoCount = render_signal_len,
			.pSignalSemaphoreInfos = render_signal,
		},
	};
	VkResult res = renderer->dev->api.vkQueueSubmit2KHR(renderer->dev->queue,
		sizeof(submit_info) / sizeof(submit_info[0]), submit_info, VK_NULL_HANDLE);
	if (res != VK_SUCCESS) {
		nx_vk_error("vkQueueSubmit", res);
		return res;
	}

	vulkan_stage_mark_submit(renderer, render_point);
	return VK_SUCCESS;
}

// One per plane per buffer.
static VkSemaphoreSubmitInfoKHR *reserve_waits(struct nx_vk_render_pass *pass) {
	size_t textures_len = pass->textures.size / sizeof(struct nx_vk_render_pass_texture);
	struct wl_array *waits = &pass->renderer->submit.waits;
	waits->size = 0;
	return wl_array_add(waits,
		(1 + textures_len) * NX_DMABUF_MAX_PLANES * sizeof(VkSemaphoreSubmitInfoKHR));
}

static VkResult submit_command_buffers(struct nx_vk_render_pass *pass,
		struct nx_vk_command_buffer *stage_cb) {
	if (pass->two_pass && !vulkan_record_output_subpass(pass)) {
		return VK_ERROR_UNKNOWN;
	}
	vkCmdEndRenderPass(pass->command_buffer->vk);

	struct nx_vk_wait_list waits = { .infos = reserve_waits(pass) };
	if (waits.infos == NULL || !vulkan_record_pass_barriers(pass, stage_cb)) {
		nx_log_errno(NX_ERROR, "Allocation failed");
		return VK_ERROR_UNKNOWN;
	}
	vulkan_collect_pass_waits(pass, &waits);
	return queue_submit(pass, stage_cb, &waits);
}

static void abort_pass(struct nx_vk_render_pass *pass, struct nx_vk_command_buffer *stage_cb,
		VkResult res) {
	struct nx_vk_renderer *renderer = pass->renderer;
	vulkan_reset_command_buffer(stage_cb);
	vulkan_reset_command_buffer(pass->command_buffer);
	nx_buffer_unlock(pass->render_buffer->nx_buffer);
	vulkan_render_pass_destroy(pass);

	if (res == VK_ERROR_DEVICE_LOST) {
		wl_signal_emit_mutable(&renderer->nx_renderer.events.lost, NULL);
	}
}

bool vulkan_render_pass_submit(struct nx_render_pass *nx_pass) {
	struct nx_vk_render_pass *pass = vulkan_get_render_pass(nx_pass);
	struct nx_vk_command_buffer *stage_cb = pass->failed ? NULL : take_stage_cb(pass->renderer);
	VkResult res = stage_cb ? submit_command_buffers(pass, stage_cb) : VK_ERROR_UNKNOWN;
	if (res != VK_SUCCESS) {
		abort_pass(pass, stage_cb, res);
		return false;
	}

	if (!vulkan_sync_render_pass_release(pass)) {
		nx_log(NX_ERROR, "Failed to sync render buffer");
	}
	struct nx_buffer *buffer = pass->render_buffer->nx_buffer;
	vulkan_render_pass_destroy(pass);
	nx_buffer_unlock(buffer);
	return true;
}
