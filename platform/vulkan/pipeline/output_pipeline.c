#include <assert.h>
#include <stdint.h>
#include <vulkan/vulkan.h>
#include "platform/log/log.h"
#include "platform/vulkan/pipeline/pipeline.h"
#include "platform/vulkan/pipeline/render_setup.h"
#include "platform/vulkan/renderer.h"

static_assert(sizeof(struct nx_vk_vert_pcr_data) +
	sizeof(struct nx_vk_frag_output_pcr_data) < 128,
	"Expected to need <= 128 bytes of push constants");

// Derived from the identity pipeline.
static const enum nx_vk_output_transform derived_transforms[] = {
	NX_VK_OUTPUT_TRANSFORM_LUT3D,
	NX_VK_OUTPUT_TRANSFORM_INVERSE_SRGB,
	NX_VK_OUTPUT_TRANSFORM_INVERSE_ST2084_PQ,
	NX_VK_OUTPUT_TRANSFORM_INVERSE_GAMMA22,
	NX_VK_OUTPUT_TRANSFORM_INVERSE_BT1886,
};

static const VkSpecializationMapEntry transform_spec_entry = {
	.constantID = 0,
	.offset = 0,
	.size = sizeof(uint32_t),
};

static bool create_ds_layout(VkDevice dev, const VkDescriptorSetLayoutBinding *binding,
		VkDescriptorSetLayout *layout) {
	VkDescriptorSetLayoutCreateInfo ds_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = 1,
		.pBindings = binding,
	};
	VkResult res = vkCreateDescriptorSetLayout(dev, &ds_info, NULL, layout);
	if (res != VK_SUCCESS) {
		nx_vk_error("vkCreateDescriptorSetLayout", res);
		return false;
	}
	return true;
}

static bool create_lut3d_sampler(struct nx_vk_renderer *renderer) {
	VkSamplerCreateInfo sampler_info = {
		.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		.magFilter = VK_FILTER_LINEAR,
		.minFilter = VK_FILTER_LINEAR,
		.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
		.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.minLod = 0.f,
		.maxLod = 0.25f,
	};
	VkResult res = vkCreateSampler(renderer->dev->dev, &sampler_info, NULL,
		&renderer->output_sampler_lut3d);
	if (res != VK_SUCCESS) {
		nx_vk_error("vkCreateSampler", res);
		return false;
	}
	return true;
}

static bool create_output_pipe_layout(struct nx_vk_renderer *renderer) {
	VkPushConstantRange pc_ranges[] = {
		{
			.offset = 0,
			.size = sizeof(struct nx_vk_vert_pcr_data),
			.stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
		},
		{
			.offset = sizeof(struct nx_vk_vert_pcr_data),
			.size = sizeof(struct nx_vk_frag_output_pcr_data),
			.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
		},
	};
	VkDescriptorSetLayout ds_layouts[] = {
		renderer->output_ds_srgb_layout,
		renderer->output_ds_lut3d_layout,
	};
	VkPipelineLayoutCreateInfo pl_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = sizeof(ds_layouts) / sizeof(ds_layouts[0]),
		.pSetLayouts = ds_layouts,
		.pushConstantRangeCount = sizeof(pc_ranges) / sizeof(pc_ranges[0]),
		.pPushConstantRanges = pc_ranges,
	};
	VkResult res = vkCreatePipelineLayout(renderer->dev->dev, &pl_info, NULL,
		&renderer->output_pipe_layout);
	if (res != VK_SUCCESS) {
		nx_vk_error("vkCreatePipelineLayout", res);
		return false;
	}
	return true;
}

// Blend input and LUT sets.
bool vulkan_init_output_layouts(struct nx_vk_renderer *renderer) {
	VkDevice dev = renderer->dev->dev;
	VkDescriptorSetLayoutBinding input_binding = {
		.binding = 0,
		.descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,
		.descriptorCount = 1,
		.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
	};
	if (!create_ds_layout(dev, &input_binding, &renderer->output_ds_srgb_layout) ||
			!create_lut3d_sampler(renderer)) {
		return false;
	}

	VkDescriptorSetLayoutBinding lut3d_binding = {
		.binding = 0,
		.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		.descriptorCount = 1,
		.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
		.pImmutableSamplers = &renderer->output_sampler_lut3d,
	};
	return create_ds_layout(dev, &lut3d_binding, &renderer->output_ds_lut3d_layout) &&
		create_output_pipe_layout(renderer);
}

static bool create_output_pipeline(struct nx_vk_render_format_setup *setup,
		enum nx_vk_output_transform transform, VkPipeline base) {
	struct nx_vk_renderer *renderer = setup->renderer;
	uint32_t output_transform_type = transform;
	VkSpecializationInfo specialization = {
		.mapEntryCount = 1,
		.pMapEntries = &transform_spec_entry,
		.dataSize = sizeof(uint32_t),
		.pData = &output_transform_type,
	};
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
			.module = renderer->output_module,
			.pName = "main",
			.pSpecializationInfo = &specialization,
		},
	};

	VkPipelineColorBlendAttachmentState blend_attachment = {
		.blendEnable = false,
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
		.flags = base == VK_NULL_HANDLE ? VK_PIPELINE_CREATE_ALLOW_DERIVATIVES_BIT :
			VK_PIPELINE_CREATE_DERIVATIVE_BIT,
		.layout = renderer->output_pipe_layout,
		.renderPass = setup->render_pass,
		.subpass = 1,
		.stageCount = sizeof(stages) / sizeof(stages[0]),
		.pStages = stages,
		.pColorBlendState = &blend,
		.basePipelineHandle = base,
		.basePipelineIndex = -1,
	};
	vulkan_pipeline_set_fixed_state(&pinfo);

	VkResult res = vkCreateGraphicsPipelines(renderer->dev->dev, renderer->pipeline_cache,
		1, &pinfo, NULL, &setup->output_pipes[transform]);
	if (res != VK_SUCCESS) {
		nx_vk_error("failed to create vulkan pipelines:", res);
		return false;
	}
	renderer->pipeline_cache_dirty = true;
	return true;
}

// Identity first, others derive.
bool vulkan_create_output_pipelines(struct nx_vk_render_format_setup *setup) {
	if (!create_output_pipeline(setup, NX_VK_OUTPUT_TRANSFORM_IDENTITY, VK_NULL_HANDLE)) {
		return false;
	}
	VkPipeline base = setup->output_pipes[NX_VK_OUTPUT_TRANSFORM_IDENTITY];
	for (size_t i = 0; i < sizeof(derived_transforms) / sizeof(derived_transforms[0]); i++) {
		if (!create_output_pipeline(setup, derived_transforms[i], base)) {
			return false;
		}
	}
	return true;
}
