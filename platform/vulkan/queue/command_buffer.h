#ifndef NX_VULKAN_QUEUE_COMMAND_BUFFER_H
#define NX_VULKAN_QUEUE_COMMAND_BUFFER_H

#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>
#include <wayland-util.h>

struct nx_vk_renderer;

struct nx_vk_command_buffer {
	VkCommandBuffer vk;
	bool recording;
	uint64_t timeline_point;
	// Destroyed once this completes.
	struct wl_list destroy_textures; // nx_vk_texture.destroy_link

	// For implicit sync; optional.
	VkSemaphore binary_semaphore;

	struct wl_array wait_semaphores; // VkSemaphore
};

// Layout, stage and access.
struct nx_vk_image_use {
	VkImageLayout layout;
	VkPipelineStageFlags stage;
	VkAccessFlags access;
};

struct nx_vk_command_buffer *vulkan_acquire_command_buffer(struct nx_vk_renderer *renderer);
uint64_t vulkan_end_command_buffer(struct nx_vk_command_buffer *cb,
	struct nx_vk_renderer *renderer);
void vulkan_reset_command_buffer(struct nx_vk_command_buffer *cb);
bool vulkan_wait_command_buffer(struct nx_vk_command_buffer *cb,
	struct nx_vk_renderer *renderer);
void vulkan_command_buffer_finish(struct nx_vk_command_buffer *cb,
	struct nx_vk_renderer *renderer);

void vulkan_change_layout(VkCommandBuffer cb, VkImage img,
	struct nx_vk_image_use from, struct nx_vk_image_use to);

#endif
