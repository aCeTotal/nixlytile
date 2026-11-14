#ifndef NX_VULKAN_IMAGE_RENDER_BUFFER_H
#define NX_VULKAN_IMAGE_RENDER_BUFFER_H

#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>
#include <wayland-util.h>
#include "platform/buffer/buffer.h"
#include "platform/container/addon.h"
#include "platform/render/dmabuf.h"
#include "platform/vulkan/pipeline/render_setup.h"

struct nx_vk_renderer;
struct nx_vk_descriptor_pool;

// One pathway's framebuffer and view.
struct nx_vk_render_target {
	VkImageView image_view;
	VkFramebuffer framebuffer;
	struct nx_vk_render_format_setup *render_setup;
	bool transitioned;
};

// An nx_buffer imported for rendering.
struct nx_vk_render_buffer {
	struct nx_buffer *nx_buffer;
	struct nx_addon addon;
	struct nx_vk_renderer *renderer;
	struct wl_list link; // nx_vk_renderer.render_buffers

	uint64_t last_timeline_point; // last render submission
	struct wl_list pending_destroy_link; // nx_vk_renderer.pending_destroy_render_buffers

	VkDeviceMemory memories[NX_DMABUF_MAX_PLANES];
	uint32_t mem_count;
	VkImage image;

	struct nx_vk_render_target targets[NX_VK_PATHWAY_COUNT];

	// Two-pass 16F blending image.
	struct {
		VkImage image;
		VkImageView image_view;
		VkDeviceMemory memory;
		VkDescriptorSet ds;
		struct nx_vk_descriptor_pool *ds_pool;
		bool transitioned;
	} blend;
};

struct nx_vk_render_buffer *vulkan_get_or_create_render_buffer(
	struct nx_vk_renderer *renderer, struct nx_buffer *nx_buffer);
void vulkan_render_buffers_reap(struct nx_vk_renderer *renderer, uint64_t current_point);
void vulkan_render_buffers_finish(struct nx_vk_renderer *renderer);

bool vulkan_setup_one_pass_framebuffer(struct nx_vk_render_buffer *buffer,
	const struct nx_dmabuf_attributes *dmabuf, enum nx_vk_pathway pathway);
bool vulkan_setup_two_pass_framebuffer(struct nx_vk_render_buffer *buffer,
	const struct nx_dmabuf_attributes *dmabuf);

#endif
