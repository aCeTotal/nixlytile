#include <assert.h>
#include <stdlib.h>
#include <vulkan/vulkan.h>
#include "platform/log/log.h"
#include "platform/vulkan/image/texture.h"
#include "platform/vulkan/pipeline/descriptor_pool.h"
#include "platform/vulkan/pipeline/pipeline.h"
#include "platform/vulkan/renderer.h"

static VkResult create_image_view(struct nx_vk_texture *texture,
		struct nx_vk_texture_view *view) {
	bool ycbcr = vulkan_format_is_ycbcr(texture->format);
	VkSamplerYcbcrConversionInfo ycbcr_conversion_info = {
		.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO,
		.conversion = view->layout->ycbcr_conversion,
	};
	assert(!ycbcr || ycbcr_conversion_info.conversion != VK_NULL_HANDLE);

	VkImageViewCreateInfo view_info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.pNext = ycbcr ? &ycbcr_conversion_info : NULL,
		.viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = view->srgb ? texture->format->vk_srgb : texture->format->vk,
		.components.r = VK_COMPONENT_SWIZZLE_IDENTITY,
		.components.g = VK_COMPONENT_SWIZZLE_IDENTITY,
		.components.b = VK_COMPONENT_SWIZZLE_IDENTITY,
		.components.a = texture->has_alpha || ycbcr ?
			VK_COMPONENT_SWIZZLE_IDENTITY : VK_COMPONENT_SWIZZLE_ONE,
		.subresourceRange = (VkImageSubresourceRange){
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.baseMipLevel = 0,
			.levelCount = 1,
			.baseArrayLayer = 0,
			.layerCount = 1,
		},
		.image = texture->image,
	};
	return vkCreateImageView(texture->renderer->dev->dev, &view_info, NULL,
		&view->image_view);
}

static struct nx_vk_texture_view *create_view(struct nx_vk_texture *texture,
		const struct nx_vk_pipeline_layout *layout, bool srgb) {
	struct nx_vk_texture_view *view = calloc(1, sizeof(*view));
	if (!view) {
		return NULL;
	}
	view->layout = layout;
	view->srgb = srgb;

	VkDevice dev = texture->renderer->dev->dev;
	VkResult res = create_image_view(texture, view);
	if (res != VK_SUCCESS) {
		free(view);
		nx_vk_error("vkCreateImageView failed", res);
		return NULL;
	}

	view->ds_pool = vulkan_alloc_texture_ds(texture->renderer, layout->ds, &view->ds);
	if (!view->ds_pool) {
		vkDestroyImageView(dev, view->image_view, NULL);
		free(view);
		nx_log(NX_ERROR, "failed to allocate descriptor");
		return NULL;
	}

	vulkan_write_sampler_descriptor(dev, view->ds, view->image_view);
	return view;
}

struct nx_vk_texture_view *vulkan_texture_get_or_create_view(struct nx_vk_texture *texture,
		const struct nx_vk_pipeline_layout *layout, bool srgb) {
	assert(texture->using_mutable_srgb || !srgb);

	struct nx_vk_texture_view *view;
	wl_list_for_each(view, &texture->views, link) {
		if (view->layout == layout && view->srgb == srgb) {
			return view;
		}
	}

	view = create_view(texture, layout, srgb);
	if (view) {
		wl_list_insert(&texture->views, &view->link);
	}
	return view;
}
