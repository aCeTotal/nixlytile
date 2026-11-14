#ifndef NX_VULKAN_RENDERER_H
#define NX_VULKAN_RENDERER_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <vulkan/vulkan.h>
#include <wayland-util.h>
#include "platform/render/renderer_interface.h"
#include "platform/vulkan/device/device.h"
#include "platform/vulkan/image/read_pixels.h"
#include "platform/vulkan/pass/lut3d.h"
#include "platform/vulkan/pipeline/descriptor_pool.h"
#include "platform/vulkan/queue/command_buffer.h"

#define VULKAN_COMMAND_BUFFERS_CAP 16

struct nx_vk_renderer {
	struct nx_renderer nx_renderer;
	struct nx_vk_device *dev;

	VkCommandPool command_pool;

	VkShaderModule vert_module;
	VkShaderModule tex_frag_module;
	VkShaderModule quad_frag_module;
	VkShaderModule output_module;

	struct wl_list pipeline_layouts; // nx_vk_pipeline_layout.link

	// Blend to output subpass.
	VkPipelineLayout output_pipe_layout;
	VkDescriptorSetLayout output_ds_srgb_layout;
	VkDescriptorSetLayout output_ds_lut3d_layout;
	VkSampler output_sampler_lut3d;
	// Placeholder for absent LUTs.
	struct nx_vk_lut3d dummy_lut3d;
	bool dummy_lut3d_transitioned;

	VkPipelineCache pipeline_cache;
	bool pipeline_cache_dirty;
	struct timespec pipeline_cache_last_save;

	VkSemaphore timeline_semaphore;
	uint64_t timeline_point;

	struct nx_vk_descriptor_pools texture_pools;
	struct nx_vk_descriptor_pools blend_pools;
	struct wl_list render_format_setups; // nx_vk_render_format_setup.link

	struct wl_list textures; // nx_vk_texture.link
	// Released to the foreign queue.
	struct wl_list foreign_textures; // nx_vk_texture.foreign_link

	struct wl_list render_buffers; // nx_vk_render_buffer.link
	struct wl_list pending_destroy_render_buffers; // nx_vk_render_buffer.pending_destroy_link

	struct wl_list color_transforms; // nx_vk_color_transform.link

	struct nx_vk_command_buffer command_buffers[VULKAN_COMMAND_BUFFERS_CAP];

	struct {
		struct nx_vk_command_buffer *cb;
		uint64_t last_timeline_point;
		struct wl_list buffers; // nx_vk_stage_buffer.link
	} stage;

	// Reused by every pass submit.
	struct {
		struct wl_array waits; // VkSemaphoreSubmitInfoKHR
		struct wl_array barriers; // VkImageMemoryBarrier
	} submit;

	struct nx_vk_readback_cache readback;
};

struct nx_vk_renderer *vulkan_get_renderer(struct nx_renderer *nx_renderer);
void vulkan_renderer_destroy(struct nx_renderer *nx_renderer);

#endif
