#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan.h>
#include "platform/log/log.h"
#include "platform/vulkan/pass/lut3d.h"
#include "platform/vulkan/pipeline/pipeline.h"
#include "platform/vulkan/renderer.h"
#include "platform/vulkan/shaders/common.vert.h"
#include "platform/vulkan/shaders/output.frag.h"
#include "platform/vulkan/shaders/quad.frag.h"
#include "platform/vulkan/shaders/texture.frag.h"

static bool create_shader_modules(struct nx_vk_renderer *renderer) {
	const struct {
		const uint32_t *code;
		size_t size;
		VkShaderModule *module;
	} shaders[] = {
		{ common_vert_data, sizeof(common_vert_data), &renderer->vert_module },
		{ texture_frag_data, sizeof(texture_frag_data), &renderer->tex_frag_module },
		{ quad_frag_data, sizeof(quad_frag_data), &renderer->quad_frag_module },
		{ output_frag_data, sizeof(output_frag_data), &renderer->output_module },
	};

	for (size_t i = 0; i < sizeof(shaders) / sizeof(shaders[0]); i++) {
		VkShaderModuleCreateInfo info = {
			.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
			.codeSize = shaders[i].size,
			.pCode = shaders[i].code,
		};
		VkResult res = vkCreateShaderModule(renderer->dev->dev, &info, NULL,
			shaders[i].module);
		if (res != VK_SUCCESS) {
			nx_vk_error("Failed to create shader module", res);
			return false;
		}
	}
	return true;
}

// The renderer frees it all.
bool vulkan_init_static_render_data(struct nx_vk_renderer *renderer) {
	return vulkan_init_output_layouts(renderer) &&
		vulkan_lut3d_init_dummy(renderer) &&
		create_shader_modules(renderer);
}
