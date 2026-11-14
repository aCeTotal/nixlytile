#include <vulkan/vulkan.h>
#include "platform/log/log.h"
#include "platform/render/color.h"
#include "platform/vulkan/pass/lut3d.h"
#include "platform/vulkan/pipeline/descriptor_pool.h"
#include "platform/vulkan/queue/command_buffer.h"
#include "platform/vulkan/queue/stage.h"
#include "platform/vulkan/renderer.h"

// RGB32F is optional in Vulkan.
static const VkFormat lut_format = VK_FORMAT_R32G32B32A32_SFLOAT;
static const size_t lut_texel_size = 4 * sizeof(float);

static const struct nx_vk_image_use undefined = {
	.layout = VK_IMAGE_LAYOUT_UNDEFINED,
	.stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
	.access = 0,
};

static const struct nx_vk_image_use transfer_dst = {
	.layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	.stage = VK_PIPELINE_STAGE_TRANSFER_BIT,
	.access = VK_ACCESS_TRANSFER_WRITE_BIT,
};

static const struct nx_vk_image_use sampled = {
	.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	.stage = VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
	.access = VK_ACCESS_SHADER_READ_BIT,
};

static bool create_image(struct nx_vk_renderer *renderer, struct nx_vk_lut3d *lut,
		VkImageUsageFlags usage) {
	VkDevice dev = renderer->dev->dev;
	VkImageCreateInfo img_info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_3D,
		.format = lut_format,
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		.extent = (VkExtent3D){ lut->dim, lut->dim, lut->dim },
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = usage,
	};
	VkResult res = vkCreateImage(dev, &img_info, NULL, &lut->image);
	if (res != VK_SUCCESS) {
		nx_vk_error("vkCreateImage failed", res);
		return false;
	}

	lut->memory = vulkan_alloc_image_memory(renderer->dev, lut->image,
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (lut->memory == VK_NULL_HANDLE) {
		return false;
	}

	VkImageViewCreateInfo view_info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.viewType = VK_IMAGE_VIEW_TYPE_3D,
		.format = lut_format,
		.components.r = VK_COMPONENT_SWIZZLE_IDENTITY,
		.components.g = VK_COMPONENT_SWIZZLE_IDENTITY,
		.components.b = VK_COMPONENT_SWIZZLE_IDENTITY,
		.components.a = VK_COMPONENT_SWIZZLE_IDENTITY,
		.subresourceRange = (VkImageSubresourceRange){
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.baseMipLevel = 0,
			.levelCount = 1,
			.baseArrayLayer = 0,
			.layerCount = 1,
		},
		.image = lut->image,
	};
	res = vkCreateImageView(dev, &view_info, NULL, &lut->image_view);
	if (res != VK_SUCCESS) {
		nx_vk_error("vkCreateImageView failed", res);
		return false;
	}
	return true;
}

static bool bind_descriptor(struct nx_vk_renderer *renderer, struct nx_vk_lut3d *lut) {
	lut->ds_pool = vulkan_alloc_texture_ds(renderer, renderer->output_ds_lut3d_layout,
		&lut->ds);
	if (!lut->ds_pool) {
		nx_log(NX_ERROR, "Failed to allocate descriptor");
		return false;
	}
	vulkan_write_sampler_descriptor(renderer->dev->dev, lut->ds, lut->image_view);
	return true;
}

// Red varies fastest.
static void fill_samples(float *dst, size_t dim, struct nx_color_transform *transform) {
	float sample_range = 1.0f / (dim - 1);
	for (size_t i = 0; i < dim * dim * dim; i++) {
		float rgb_in[3] = {
			(i % dim) * sample_range,
			(i / dim % dim) * sample_range,
			(i / (dim * dim)) * sample_range,
		};
		float rgb_out[3];
		nx_color_transform_eval(transform, rgb_out, rgb_in);

		dst[4 * i] = rgb_out[0];
		dst[4 * i + 1] = rgb_out[1];
		dst[4 * i + 2] = rgb_out[2];
		dst[4 * i + 3] = 1.0f;
	}
}

static bool upload(struct nx_vk_renderer *renderer, struct nx_vk_lut3d *lut,
		struct nx_color_transform *transform) {
	size_t size = lut->dim * lut->dim * lut->dim * lut_texel_size;
	struct nx_vk_buffer_span span = vulkan_get_stage_span(renderer, size, lut_texel_size);
	if (!span.buffer || span.size != size) {
		nx_log(NX_ERROR, "Failed to retrieve staging buffer");
		return false;
	}
	fill_samples(vulkan_span_map(&span), lut->dim, transform);

	VkCommandBuffer cb = vulkan_record_stage_cb(renderer);
	vulkan_change_layout(cb, lut->image, undefined, transfer_dst);
	VkBufferImageCopy copy = {
		.bufferOffset = span.offset,
		.imageExtent.width = lut->dim,
		.imageExtent.height = lut->dim,
		.imageExtent.depth = lut->dim,
		.imageSubresource.layerCount = 1,
		.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
	};
	vkCmdCopyBufferToImage(cb, span.buffer->buffer, lut->image,
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
	vulkan_change_layout(cb, lut->image, transfer_dst, sampled);
	return true;
}

bool vulkan_lut3d_bake(struct nx_vk_renderer *renderer, struct nx_vk_lut3d *lut,
		struct nx_color_transform *transform) {
	if (create_image(renderer, lut, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT) &&
			upload(renderer, lut, transform) && bind_descriptor(renderer, lut)) {
		return true;
	}
	vulkan_lut3d_finish(lut, renderer->dev->dev);
	return false;
}

// Shaders always need a LUT.
bool vulkan_lut3d_init_dummy(struct nx_vk_renderer *renderer) {
	struct nx_vk_lut3d *lut = &renderer->dummy_lut3d;
	lut->dim = 1;
	return create_image(renderer, lut, VK_IMAGE_USAGE_SAMPLED_BIT) &&
		bind_descriptor(renderer, lut);
}

void vulkan_lut3d_finish(struct nx_vk_lut3d *lut, VkDevice dev) {
	vkDestroyImageView(dev, lut->image_view, NULL);
	vkDestroyImage(dev, lut->image, NULL);
	vkFreeMemory(dev, lut->memory, NULL);
}
