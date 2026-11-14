#include <stdlib.h>
#include <vulkan/vulkan.h>
#include "platform/log/log.h"
#include "platform/vulkan/pipeline/render_setup.h"
#include "platform/vulkan/renderer.h"

// Prebuilt to spare first frames.
static const struct nx_vk_pipeline_key default_pipelines[] = {
	{ .source = NX_VK_SHADER_SOURCE_SINGLE_COLOR },
	{
		.source = NX_VK_SHADER_SOURCE_TEXTURE,
		.texture_transform = NX_VK_TEXTURE_TRANSFORM_IDENTITY,
	},
	{
		.source = NX_VK_SHADER_SOURCE_TEXTURE,
		.texture_transform = NX_VK_TEXTURE_TRANSFORM_SRGB,
	},
};

static bool create_render_passes(struct nx_vk_render_format_setup *setup) {
	if (setup->pathway != NX_VK_PATHWAY_TWO_PASS) {
		return vulkan_create_one_pass_render_pass(setup);
	}
	return vulkan_create_two_pass_render_pass(setup) &&
		vulkan_create_output_pipelines(setup);
}

static bool create_default_pipelines(struct nx_vk_render_format_setup *setup) {
	for (size_t i = 0; i < sizeof(default_pipelines) / sizeof(default_pipelines[0]); i++) {
		if (!vulkan_get_or_create_pipeline(setup, &default_pipelines[i])) {
			return false;
		}
	}
	return true;
}

struct nx_vk_render_format_setup *vulkan_find_or_create_render_setup(
		struct nx_vk_renderer *renderer, const struct nx_vk_format *format,
		enum nx_vk_pathway pathway) {
	struct nx_vk_render_format_setup *setup;
	wl_list_for_each(setup, &renderer->render_format_setups, link) {
		if (setup->render_format == format && setup->pathway == pathway) {
			return setup;
		}
	}

	setup = calloc(1u, sizeof(*setup));
	if (!setup) {
		nx_log(NX_ERROR, "Allocation failed");
		return NULL;
	}
	setup->render_format = format;
	setup->pathway = pathway;
	setup->renderer = renderer;
	wl_list_init(&setup->pipelines);

	if (!create_render_passes(setup) || !create_default_pipelines(setup)) {
		vulkan_destroy_render_setup(setup);
		return NULL;
	}
	wl_list_insert(&renderer->render_format_setups, &setup->link);
	return setup;
}

void vulkan_destroy_render_setup(struct nx_vk_render_format_setup *setup) {
	VkDevice dev = setup->renderer->dev->dev;
	vkDestroyRenderPass(dev, setup->render_pass, NULL);
	vkDestroyRenderPass(dev, setup->render_pass_clear, NULL);
	for (size_t i = 0; i < NX_VK_OUTPUT_TRANSFORM_COUNT; i++) {
		vkDestroyPipeline(dev, setup->output_pipes[i], NULL);
	}

	struct nx_vk_pipeline *pipeline, *tmp;
	wl_list_for_each_safe(pipeline, tmp, &setup->pipelines, link) {
		vkDestroyPipeline(dev, pipeline->vk, NULL);
		free(pipeline);
	}
	free(setup);
}
