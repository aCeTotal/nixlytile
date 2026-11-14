#include <assert.h>
#include <stdint.h>
#include <vulkan/vulkan.h>
#include "platform/log/log.h"
#include "platform/vulkan/image/render_buffer.h"
#include "platform/vulkan/image/texture.h"
#include "platform/vulkan/pipeline/pipeline_cache.h"
#include "platform/vulkan/queue/command_buffer.h"
#include "platform/vulkan/queue/stage.h"
#include "platform/vulkan/renderer.h"

static bool init_command_buffer(struct nx_vk_command_buffer *cb,
		struct nx_vk_renderer *renderer) {
	VkCommandBuffer vk_cb = VK_NULL_HANDLE;
	VkCommandBufferAllocateInfo cmd_buf_info = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = renderer->command_pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	};
	VkResult res = vkAllocateCommandBuffers(renderer->dev->dev, &cmd_buf_info, &vk_cb);
	if (res != VK_SUCCESS) {
		nx_vk_error("vkAllocateCommandBuffers", res);
		return false;
	}

	*cb = (struct nx_vk_command_buffer){
		.vk = vk_cb,
	};
	wl_list_init(&cb->destroy_textures);
	return true;
}

static void release_resources(struct nx_vk_command_buffer *cb) {
	struct nx_vk_texture *texture, *tmp;
	wl_list_for_each_safe(texture, tmp, &cb->destroy_textures, destroy_link) {
		wl_list_remove(&texture->destroy_link);
		texture->last_used_cb = NULL;
		nx_texture_destroy(&texture->nx_texture);
	}
}

static bool is_done(const struct nx_vk_command_buffer *cb, uint64_t current_point) {
	return cb->vk != VK_NULL_HANDLE && !cb->recording && cb->timeline_point <= current_point;
}

static void collect_garbage(struct nx_vk_renderer *renderer, uint64_t current_point) {
	vulkan_stage_buffer_gc(renderer, current_point);
	vulkan_pipeline_cache_tick(renderer);

	for (size_t i = 0; i < VULKAN_COMMAND_BUFFERS_CAP; i++) {
		struct nx_vk_command_buffer *cb = &renderer->command_buffers[i];
		if (is_done(cb, current_point)) {
			release_resources(cb);
		}
	}
	vulkan_render_buffers_reap(renderer, current_point);
}

static struct nx_vk_command_buffer *get_command_buffer(struct nx_vk_renderer *renderer) {
	uint64_t current_point;
	VkResult res = renderer->dev->api.vkGetSemaphoreCounterValueKHR(renderer->dev->dev,
		renderer->timeline_semaphore, &current_point);
	if (res != VK_SUCCESS) {
		nx_vk_error("vkGetSemaphoreCounterValueKHR", res);
		return NULL;
	}

	collect_garbage(renderer, current_point);

	struct nx_vk_command_buffer *oldest = NULL;
	for (size_t i = 0; i < VULKAN_COMMAND_BUFFERS_CAP; i++) {
		struct nx_vk_command_buffer *cb = &renderer->command_buffers[i];
		if (cb->vk == VK_NULL_HANDLE) {
			return init_command_buffer(cb, renderer) ? cb : NULL;
		}
		if (cb->recording) {
			continue;
		}
		if (cb->timeline_point <= current_point) {
			return cb;
		}
		if (oldest == NULL || cb->timeline_point < oldest->timeline_point) {
			oldest = cb;
		}
	}

	// All busy: wait for oldest.
	return vulkan_wait_command_buffer(oldest, renderer) ? oldest : NULL;
}

struct nx_vk_command_buffer *vulkan_acquire_command_buffer(struct nx_vk_renderer *renderer) {
	struct nx_vk_command_buffer *cb = get_command_buffer(renderer);
	if (cb == NULL) {
		return NULL;
	}

	assert(!cb->recording);
	cb->recording = true;
	return cb;
}

uint64_t vulkan_end_command_buffer(struct nx_vk_command_buffer *cb,
		struct nx_vk_renderer *renderer) {
	assert(cb->recording);
	cb->recording = false;

	VkResult res = vkEndCommandBuffer(cb->vk);
	if (res != VK_SUCCESS) {
		nx_vk_error("vkEndCommandBuffer", res);
		return 0;
	}

	renderer->timeline_point++;
	cb->timeline_point = renderer->timeline_point;
	return cb->timeline_point;
}

void vulkan_reset_command_buffer(struct nx_vk_command_buffer *cb) {
	if (cb == NULL) {
		return;
	}

	cb->recording = false;
	VkResult res = vkResetCommandBuffer(cb->vk, 0);
	if (res != VK_SUCCESS) {
		nx_vk_error("vkResetCommandBuffer", res);
	}
}

bool vulkan_wait_command_buffer(struct nx_vk_command_buffer *cb,
		struct nx_vk_renderer *renderer) {
	assert(cb->vk != VK_NULL_HANDLE && !cb->recording);

	VkSemaphoreWaitInfoKHR wait_info = {
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO_KHR,
		.semaphoreCount = 1,
		.pSemaphores = &renderer->timeline_semaphore,
		.pValues = &cb->timeline_point,
	};
	VkResult res = renderer->dev->api.vkWaitSemaphoresKHR(renderer->dev->dev,
		&wait_info, UINT64_MAX);
	if (res != VK_SUCCESS) {
		nx_vk_error("vkWaitSemaphoresKHR", res);
		return false;
	}
	return true;
}

// The pool frees the buffer.
void vulkan_command_buffer_finish(struct nx_vk_command_buffer *cb,
		struct nx_vk_renderer *renderer) {
	if (cb->vk == VK_NULL_HANDLE) {
		return;
	}

	VkDevice dev = renderer->dev->dev;
	release_resources(cb);
	vkDestroySemaphore(dev, cb->binary_semaphore, NULL);
	VkSemaphore *sem;
	wl_array_for_each(sem, &cb->wait_semaphores) {
		vkDestroySemaphore(dev, *sem, NULL);
	}
	wl_array_release(&cb->wait_semaphores);
}

void vulkan_change_layout(VkCommandBuffer cb, VkImage img,
		struct nx_vk_image_use from, struct nx_vk_image_use to) {
	VkImageMemoryBarrier barrier = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.oldLayout = from.layout,
		.newLayout = to.layout,
		.image = img,
		.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
		.subresourceRange.layerCount = 1,
		.subresourceRange.levelCount = 1,
		.srcAccessMask = from.access,
		.dstAccessMask = to.access,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	};
	vkCmdPipelineBarrier(cb, from.stage, to.stage, 0, 0, NULL, 0, NULL, 1, &barrier);
}
