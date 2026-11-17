#include <assert.h>
#include <stdlib.h>
#include <vulkan/vulkan.h>
#include "platform/log/log.h"
#include "platform/vulkan/pipeline/pipeline.h"
#include "platform/vulkan/renderer.h"

static_assert(sizeof(struct nx_vk_vert_pcr_data) == 48,
	"Shaders expect fragment push constants at offset 48");
static_assert(sizeof(struct nx_vk_vert_pcr_data) +
	sizeof(struct nx_vk_frag_texture_pcr_data) <= 128,
	"Expected to need <= 128 bytes of push constants");
static_assert(sizeof(struct nx_vk_frag_quad_pcr_data) <=
	sizeof(struct nx_vk_frag_texture_pcr_data),
	"The fragment range covers both shaders");

// Set and pipeline layouts.
static bool init_tex_layouts(VkDevice dev, struct nx_vk_pipeline_layout *layout) {
	VkDescriptorSetLayoutBinding ds_binding = {
		.binding = 0,
		.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		.descriptorCount = 1,
		.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
		.pImmutableSamplers = &layout->sampler,
	};
	VkDescriptorSetLayoutCreateInfo ds_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = 1,
		.pBindings = &ds_binding,
	};
	VkResult res = vkCreateDescriptorSetLayout(dev, &ds_info, NULL, &layout->ds);
	if (res != VK_SUCCESS) {
		nx_vk_error("vkCreateDescriptorSetLayout", res);
		return false;
	}

	VkPushConstantRange pc_ranges[] = {
		{
			.size = sizeof(struct nx_vk_vert_pcr_data),
			.stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
		},
		{
			.offset = sizeof(struct nx_vk_vert_pcr_data),
			.size = sizeof(struct nx_vk_frag_texture_pcr_data),
			.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
		},
	};
	VkPipelineLayoutCreateInfo pl_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 1,
		.pSetLayouts = &layout->ds,
		.pushConstantRangeCount = sizeof(pc_ranges) / sizeof(pc_ranges[0]),
		.pPushConstantRanges = pc_ranges,
	};
	res = vkCreatePipelineLayout(dev, &pl_info, NULL, &layout->vk);
	if (res != VK_SUCCESS) {
		nx_vk_error("vkCreatePipelineLayout", res);
		return false;
	}
	return true;
}

bool vulkan_pipeline_layout_key_equals(const struct nx_vk_pipeline_layout_key *a,
		const struct nx_vk_pipeline_layout_key *b) {
	assert(!a->ycbcr.format || vulkan_format_is_ycbcr(a->ycbcr.format));
	assert(!b->ycbcr.format || vulkan_format_is_ycbcr(b->ycbcr.format));

	return a->filter_mode == b->filter_mode &&
		a->ycbcr.format == b->ycbcr.format &&
		a->ycbcr.encoding == b->ycbcr.encoding &&
		a->ycbcr.range == b->ycbcr.range;
}

static VkSamplerYcbcrModelConversion ycbcr_model_from_nx(enum nx_color_encoding encoding) {
	switch (encoding) {
	case NX_COLOR_ENCODING_BT601:
		return VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_601;
	case NX_COLOR_ENCODING_BT709:
		return VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709;
	case NX_COLOR_ENCODING_BT2020:
		return VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_2020;
	default:
		abort(); // must be explicit and supported
	}
}

static VkSamplerYcbcrRange ycbcr_range_from_nx(enum nx_color_range range) {
	switch (range) {
	case NX_COLOR_RANGE_LIMITED:
		return VK_SAMPLER_YCBCR_RANGE_ITU_NARROW;
	case NX_COLOR_RANGE_FULL:
		return VK_SAMPLER_YCBCR_RANGE_ITU_FULL;
	case NX_COLOR_RANGE_NONE:
		break;
	}
	abort(); // must be explicit
}

static bool create_ycbcr_conversion(VkDevice dev, struct nx_vk_pipeline_layout *layout) {
	const struct nx_vk_pipeline_layout_key *key = &layout->key;
	VkSamplerYcbcrConversionCreateInfo conversion_info = {
		.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO,
		.format = key->ycbcr.format->vk,
		.ycbcrModel = ycbcr_model_from_nx(key->ycbcr.encoding),
		.ycbcrRange = ycbcr_range_from_nx(key->ycbcr.range),
		.xChromaOffset = VK_CHROMA_LOCATION_MIDPOINT,
		.yChromaOffset = VK_CHROMA_LOCATION_MIDPOINT,
		.chromaFilter = VK_FILTER_LINEAR,
	};
	VkResult res = vkCreateSamplerYcbcrConversion(dev, &conversion_info, NULL,
		&layout->ycbcr_conversion);
	if (res != VK_SUCCESS) {
		nx_vk_error("vkCreateSamplerYcbcrConversion", res);
		return false;
	}
	return true;
}

static bool create_sampler(VkDevice dev, struct nx_vk_pipeline_layout *layout) {
	const struct nx_vk_pipeline_layout_key *key = &layout->key;
	VkFilter filter = key->filter_mode == NX_SCALE_FILTER_NEAREST ?
		VK_FILTER_NEAREST : VK_FILTER_LINEAR;
	VkSamplerYcbcrConversionInfo conversion_info = {
		.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO,
		.conversion = layout->ycbcr_conversion,
	};
	VkSamplerCreateInfo sampler_info = {
		.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		.pNext = key->ycbcr.format ? &conversion_info : NULL,
		.magFilter = filter,
		.minFilter = filter,
		.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
		.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.minLod = 0.f,
		.maxLod = 0.25f,
	};
	VkResult res = vkCreateSampler(dev, &sampler_info, NULL, &layout->sampler);
	if (res != VK_SUCCESS) {
		nx_vk_error("vkCreateSampler", res);
		return false;
	}
	return true;
}

static bool init_pipeline_layout(VkDevice dev, struct nx_vk_pipeline_layout *layout) {
	const struct nx_vk_pipeline_layout_key *key = &layout->key;
	if (key->ycbcr.format) {
		return create_ycbcr_conversion(dev, layout) && create_sampler(dev, layout) &&
			init_tex_layouts(dev, layout);
	}

	assert(key->ycbcr.encoding == NX_COLOR_ENCODING_NONE ||
		key->ycbcr.encoding == NX_COLOR_ENCODING_IDENTITY);
	assert(key->ycbcr.range == NX_COLOR_RANGE_NONE ||
		key->ycbcr.range == NX_COLOR_RANGE_FULL);
	return create_sampler(dev, layout) && init_tex_layouts(dev, layout);
}

struct nx_vk_pipeline_layout *vulkan_get_or_create_pipeline_layout(
		struct nx_vk_renderer *renderer, const struct nx_vk_pipeline_layout_key *key) {
	struct nx_vk_pipeline_layout *layout;
	wl_list_for_each(layout, &renderer->pipeline_layouts, link) {
		if (vulkan_pipeline_layout_key_equals(&layout->key, key)) {
			return layout;
		}
	}

	layout = calloc(1, sizeof(*layout));
	if (!layout) {
		return NULL;
	}
	layout->key = *key;

	if (!init_pipeline_layout(renderer->dev->dev, layout)) {
		free(layout);
		return NULL;
	}
	wl_list_insert(&renderer->pipeline_layouts, &layout->link);
	return layout;
}

void vulkan_pipeline_layouts_finish(struct nx_vk_renderer *renderer) {
	VkDevice dev = renderer->dev->dev;
	struct nx_vk_pipeline_layout *layout, *tmp;
	wl_list_for_each_safe(layout, tmp, &renderer->pipeline_layouts, link) {
		vkDestroyPipelineLayout(dev, layout->vk, NULL);
		vkDestroyDescriptorSetLayout(dev, layout->ds, NULL);
		vkDestroySampler(dev, layout->sampler, NULL);
		vkDestroySamplerYcbcrConversion(dev, layout->ycbcr_conversion, NULL);
		free(layout);
	}
}
