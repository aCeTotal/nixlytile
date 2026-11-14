#ifndef NX_VULKAN_PIPELINE_RENDER_SETUP_H
#define NX_VULKAN_PIPELINE_RENDER_SETUP_H

#include <stdbool.h>
#include <vulkan/vulkan.h>
#include <wayland-util.h>
#include "platform/vulkan/device/format.h"
#include "platform/vulkan/pipeline/pipeline.h"

struct nx_vk_renderer;

// How passes reach the buffer.
enum nx_vk_pathway {
	NX_VK_PATHWAY_LINEAR, // direct, no transfer function
	NX_VK_PATHWAY_SRGB, // direct through an _SRGB view
	NX_VK_PATHWAY_TWO_PASS, // blend image, then output subpass
	NX_VK_PATHWAY_COUNT,
};

// Per format and pathway.
struct nx_vk_render_format_setup {
	struct wl_list link; // nx_vk_renderer.render_format_setups
	const struct nx_vk_format *render_format;
	enum nx_vk_pathway pathway;
	VkRenderPass render_pass;
	VkRenderPass render_pass_clear; // two-pass first use

	VkPipeline output_pipes[NX_VK_OUTPUT_TRANSFORM_COUNT];

	struct nx_vk_renderer *renderer;
	struct wl_list pipelines; // nx_vk_pipeline.link
};

struct nx_vk_render_format_setup *vulkan_find_or_create_render_setup(
	struct nx_vk_renderer *renderer, const struct nx_vk_format *format,
	enum nx_vk_pathway pathway);
void vulkan_destroy_render_setup(struct nx_vk_render_format_setup *setup);

bool vulkan_create_two_pass_render_pass(struct nx_vk_render_format_setup *setup);
bool vulkan_create_one_pass_render_pass(struct nx_vk_render_format_setup *setup);

#endif
