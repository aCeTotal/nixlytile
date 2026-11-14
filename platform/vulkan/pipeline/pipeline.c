#include <stdint.h>
#include <stdlib.h>
#include <vulkan/vulkan.h>
#include "platform/log/log.h"
#include "platform/vulkan/pipeline/pipeline.h"
#include "platform/vulkan/pipeline/render_setup.h"
#include "platform/vulkan/renderer.h"

static const VkSpecializationMapEntry transform_spec_entry = {
	.constantID = 0,
	.offset = 0,
	.size = sizeof(uint32_t),
};

static bool pipeline_key_equals(const struct nx_vk_pipeline_key *a,
		const struct nx_vk_pipeline_key *b) {
	if (!vulkan_pipeline_layout_key_equals(&a->layout, &b->layout) ||
			a->blend_mode != b->blend_mode || a->source != b->source) {
		return false;
	}
	return a->source != NX_VK_SHADER_SOURCE_TEXTURE ||
		a->texture_transform == b->texture_transform;
}

static VkResult create_pipeline(struct nx_vk_render_format_setup *setup,
		struct nx_vk_pipeline *pipeline) {
	struct nx_vk_renderer *renderer = setup->renderer;
	uint32_t color_transform_type = pipeline->key.texture_transform;
	VkSpecializationInfo specialization = {
		.mapEntryCount = 1,
		.pMapEntries = &transform_spec_entry,
		.dataSize = sizeof(uint32_t),
		.pData = &color_transform_type,
	};

	bool texture = pipeline->key.source == NX_VK_SHADER_SOURCE_TEXTURE;
	VkPipelineShaderStageCreateInfo stages[] = {
		{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_VERTEX_BIT,
			.module = renderer->vert_module,
			.pName = "main",
		},
		{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_FRAGMENT_BIT,
			.module = texture ? renderer->tex_frag_module : renderer->quad_frag_module,
			.pName = "main",
			.pSpecializationInfo = texture ? &specialization : NULL,
		},
	};

	// Alpha is premultiplied.
	VkPipelineColorBlendAttachmentState blend_attachment = {
		.blendEnable = pipeline->key.blend_mode == NX_RENDER_BLEND_MODE_PREMULTIPLIED,
		.srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
		.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
		.colorBlendOp = VK_BLEND_OP_ADD,
		.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
		.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
		.alphaBlendOp = VK_BLEND_OP_ADD,
		.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
			VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
	};
	VkPipelineColorBlendStateCreateInfo blend = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		.attachmentCount = 1,
		.pAttachments = &blend_attachment,
	};

	VkGraphicsPipelineCreateInfo pinfo = {
		.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		.layout = pipeline->layout->vk,
		.renderPass = setup->render_pass,
		.subpass = 0,
		.stageCount = sizeof(stages) / sizeof(stages[0]),
		.pStages = stages,
		.pColorBlendState = &blend,
	};
	vulkan_pipeline_set_fixed_state(&pinfo);

	return vkCreateGraphicsPipelines(renderer->dev->dev, renderer->pipeline_cache,
		1, &pinfo, NULL, &pipeline->vk);
}

struct nx_vk_pipeline *vulkan_get_or_create_pipeline(struct nx_vk_render_format_setup *setup,
		const struct nx_vk_pipeline_key *key) {
	struct nx_vk_pipeline *pipeline;
	wl_list_for_each(pipeline, &setup->pipelines, link) {
		if (pipeline_key_equals(&pipeline->key, key)) {
			return pipeline;
		}
	}

	struct nx_vk_renderer *renderer = setup->renderer;
	struct nx_vk_pipeline_layout *layout =
		vulkan_get_or_create_pipeline_layout(renderer, &key->layout);
	if (!layout) {
		return NULL;
	}

	pipeline = calloc(1, sizeof(*pipeline));
	if (!pipeline) {
		return NULL;
	}
	pipeline->key = *key;
	pipeline->layout = layout;

	VkResult res = create_pipeline(setup, pipeline);
	if (res != VK_SUCCESS) {
		nx_vk_error("failed to create vulkan pipelines:", res);
		free(pipeline);
		return NULL;
	}
	renderer->pipeline_cache_dirty = true;

	wl_list_insert(&setup->pipelines, &pipeline->link);
	return pipeline;
}
