#include <stdlib.h>
#include <vulkan/vulkan.h>
#include "platform/log/log.h"
#include "platform/vulkan/image/read_pixels.h"
#include "platform/vulkan/image/render_buffer.h"
#include "platform/vulkan/image/texture.h"
#include "platform/vulkan/pass/color_transform.h"
#include "platform/vulkan/pipeline/pipeline.h"
#include "platform/vulkan/pipeline/pipeline_cache.h"
#include "platform/vulkan/pipeline/render_setup.h"
#include "platform/vulkan/queue/stage.h"
#include "platform/vulkan/renderer.h"

static void destroy_resources(struct nx_vk_renderer *renderer) {
	for (size_t i = 0; i < VULKAN_COMMAND_BUFFERS_CAP; i++) {
		vulkan_command_buffer_finish(&renderer->command_buffers[i], renderer);
	}
	vulkan_stage_buffers_finish(renderer);

	struct nx_vk_texture *texture, *texture_tmp;
	wl_list_for_each_safe(texture, texture_tmp, &renderer->textures, link) {
		vulkan_texture_destroy(texture);
	}
	vulkan_render_buffers_finish(renderer);

	struct nx_vk_color_transform *transform, *transform_tmp;
	wl_list_for_each_safe(transform, transform_tmp, &renderer->color_transforms, link) {
		vulkan_color_transform_destroy(&transform->addon);
	}

	struct nx_vk_render_format_setup *setup, *setup_tmp;
	wl_list_for_each_safe(setup, setup_tmp, &renderer->render_format_setups, link) {
		vulkan_destroy_render_setup(setup);
	}

	wl_array_release(&renderer->submit.waits);
	wl_array_release(&renderer->submit.barriers);
}

static void destroy_static_objects(struct nx_vk_renderer *renderer) {
	VkDevice dev = renderer->dev->dev;
	vulkan_descriptor_pools_finish(&renderer->texture_pools, dev);
	vulkan_descriptor_pools_finish(&renderer->blend_pools, dev);

	vkDestroyShaderModule(dev, renderer->vert_module, NULL);
	vkDestroyShaderModule(dev, renderer->tex_frag_module, NULL);
	vkDestroyShaderModule(dev, renderer->quad_frag_module, NULL);
	vkDestroyShaderModule(dev, renderer->output_module, NULL);

	vulkan_pipeline_layouts_finish(renderer);
	vulkan_lut3d_finish(&renderer->dummy_lut3d, dev);

	vulkan_pipeline_cache_save(renderer);
	vkDestroyPipelineCache(dev, renderer->pipeline_cache, NULL);
	vkDestroySemaphore(dev, renderer->timeline_semaphore, NULL);
	vkDestroyPipelineLayout(dev, renderer->output_pipe_layout, NULL);
	vkDestroyDescriptorSetLayout(dev, renderer->output_ds_srgb_layout, NULL);
	vkDestroyDescriptorSetLayout(dev, renderer->output_ds_lut3d_layout, NULL);
	vkDestroyCommandPool(dev, renderer->command_pool, NULL);
	vkDestroySampler(dev, renderer->output_sampler_lut3d, NULL);
	vulkan_readback_finish(renderer);
}

// Idles the device first.
void vulkan_renderer_destroy(struct nx_renderer *nx_renderer) {
	struct nx_vk_renderer *renderer = vulkan_get_renderer(nx_renderer);
	struct nx_vk_device *dev = renderer->dev;

	VkResult res = vkDeviceWaitIdle(dev->dev);
	if (res != VK_SUCCESS) {
		nx_vk_error("vkDeviceWaitIdle", res);
	}

	destroy_resources(renderer);
	destroy_static_objects(renderer);

	struct nx_vk_instance *ini = dev->instance;
	vulkan_device_destroy(dev);
	vulkan_instance_destroy(ini);
	free(renderer);
}
