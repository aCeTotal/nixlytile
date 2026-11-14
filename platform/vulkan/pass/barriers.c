#include <vulkan/vulkan.h>
#include "platform/vulkan/image/render_buffer.h"
#include "platform/vulkan/image/texture.h"
#include "platform/vulkan/pass/pass.h"
#include "platform/vulkan/queue/command_buffer.h"
#include "platform/vulkan/renderer.h"

// Foreign image ownership transfers.
struct ownership_barriers {
	VkImageMemoryBarrier *acquire;
	VkImageMemoryBarrier *release;
	uint32_t len;
	uint32_t queue_family;
};

// Layouts and access while owned.
struct image_hold {
	VkImageLayout acquire_from;
	VkImageLayout layout;
	VkImageLayout release_to;
	VkAccessFlags access;
};

static const VkImageSubresourceRange color_range = {
	.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
	.levelCount = 1,
	.layerCount = 1,
};

static const struct image_hold texture_hold = {
	.acquire_from = VK_IMAGE_LAYOUT_GENERAL,
	.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	.release_to = VK_IMAGE_LAYOUT_GENERAL,
	.access = VK_ACCESS_SHADER_READ_BIT,
};

static void add_ownership_barriers(struct ownership_barriers *barriers, VkImage image,
		const struct image_hold *hold) {
	barriers->acquire[barriers->len] = (VkImageMemoryBarrier){
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT,
		.dstQueueFamilyIndex = barriers->queue_family,
		.image = image,
		.oldLayout = hold->acquire_from,
		.newLayout = hold->layout,
		.srcAccessMask = 0,
		.dstAccessMask = hold->access,
		.subresourceRange = color_range,
	};
	barriers->release[barriers->len] = (VkImageMemoryBarrier){
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.srcQueueFamilyIndex = barriers->queue_family,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT,
		.image = image,
		.oldLayout = hold->layout,
		.newLayout = hold->release_to,
		.srcAccessMask = hold->access,
		.dstAccessMask = 0,
		.subresourceRange = color_range,
	};
	barriers->len++;
}

// Returned after this pass.
static void take_foreign_textures(struct nx_vk_renderer *renderer,
		struct ownership_barriers *barriers) {
	struct nx_vk_texture *texture, *tmp;
	wl_list_for_each_safe(texture, tmp, &renderer->foreign_textures, foreign_link) {
		add_ownership_barriers(barriers, texture->image, &texture_hold);
		wl_list_remove(&texture->foreign_link);
		texture->owned = false;
	}
}

static void add_render_buffer_barriers(struct nx_vk_render_pass *pass,
		struct ownership_barriers *barriers) {
	struct nx_vk_render_target *target = pass->render_target;
	struct image_hold hold = {
		.acquire_from = target->transitioned ?
			VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_PREINITIALIZED,
		.layout = VK_IMAGE_LAYOUT_GENERAL,
		.release_to = VK_IMAGE_LAYOUT_GENERAL,
		.access = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
			VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
	};
	target->transitioned = true;
	add_ownership_barriers(barriers, pass->render_buffer->image, &hold);
}

// Last frame left it shader-readable.
static void reacquire_blend_image(struct nx_vk_render_pass *pass,
		struct nx_vk_command_buffer *stage_cb) {
	struct nx_vk_render_buffer *buffer = pass->render_buffer;
	if (!pass->two_pass) {
		return;
	}
	// First pass cleared it.
	if (!buffer->blend.transitioned) {
		buffer->blend.transitioned = true;
		return;
	}

	VkImageMemoryBarrier barrier = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = buffer->blend.image,
		.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		.srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
		.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		.subresourceRange = color_range,
	};
	vkCmdPipelineBarrier(stage_cb->vk, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
}

// Acquire before, release after rendering.
bool vulkan_record_pass_barriers(struct nx_vk_render_pass *pass,
		struct nx_vk_command_buffer *stage_cb) {
	struct nx_vk_renderer *renderer = pass->renderer;
	uint32_t count = wl_list_length(&renderer->foreign_textures) + 1;
	renderer->submit.barriers.size = 0;
	VkImageMemoryBarrier *acquire =
		wl_array_add(&renderer->submit.barriers, 2 * count * sizeof(*acquire));
	if (acquire == NULL) {
		return false;
	}
	VkImageMemoryBarrier *release = acquire + count;
	struct ownership_barriers barriers = {
		.acquire = acquire,
		.release = release,
		.queue_family = renderer->dev->queue_family,
	};
	take_foreign_textures(renderer, &barriers);
	add_render_buffer_barriers(pass, &barriers);
	reacquire_blend_image(pass, stage_cb);

	vkCmdPipelineBarrier(stage_cb->vk, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		0, 0, NULL, 0, NULL, count, acquire);
	vkCmdPipelineBarrier(pass->command_buffer->vk,
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, count, release);
	return true;
}
