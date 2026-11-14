#include <assert.h>
#include <vulkan/vulkan.h>
#include "platform/log/log.h"
#include "platform/vulkan/image/render_buffer.h"
#include "platform/vulkan/pipeline/descriptor_pool.h"
#include "platform/vulkan/renderer.h"

// Linear blending happens in 16F.
static const VkFormat blend_format = VK_FORMAT_R16G16B16A16_SFLOAT;

static VkImageView create_view(VkDevice dev, VkImage image, VkFormat format) {
	VkImageViewCreateInfo view_info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = image,
		.viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = format,
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
	};

	VkImageView view = VK_NULL_HANDLE;
	VkResult res = vkCreateImageView(dev, &view_info, NULL, &view);
	if (res != VK_SUCCESS) {
		nx_vk_error("vkCreateImageView failed", res);
		return VK_NULL_HANDLE;
	}
	return view;
}

// Blend image first when present.
struct attachment_list {
	VkImageView views[2];
	uint32_t len;
};

static bool create_framebuffer(struct nx_vk_render_target *target,
		const struct nx_dmabuf_attributes *dmabuf, const struct attachment_list *attachments) {
	VkFramebufferCreateInfo fb_info = {
		.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
		.attachmentCount = attachments->len,
		.pAttachments = attachments->views,
		.flags = 0u,
		.width = dmabuf->width,
		.height = dmabuf->height,
		.layers = 1u,
		.renderPass = target->render_setup->render_pass,
	};
	VkResult res = vkCreateFramebuffer(target->render_setup->renderer->dev->dev, &fb_info, NULL,
		&target->framebuffer);
	if (res != VK_SUCCESS) {
		nx_vk_error("vkCreateFramebuffer", res);
		return false;
	}
	return true;
}

static bool create_blend_image(struct nx_vk_render_buffer *buffer,
		const struct nx_dmabuf_attributes *dmabuf) {
	struct nx_vk_renderer *renderer = buffer->renderer;
	VkDevice dev = renderer->dev->dev;
	VkImageCreateInfo img_info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = blend_format,
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		.extent = (VkExtent3D){ dmabuf->width, dmabuf->height, 1 },
		.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT,
	};
	VkResult res = vkCreateImage(dev, &img_info, NULL, &buffer->blend.image);
	if (res != VK_SUCCESS) {
		nx_vk_error("vkCreateImage failed", res);
		return false;
	}

	buffer->blend.memory = vulkan_alloc_image_memory(renderer->dev, buffer->blend.image,
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (buffer->blend.memory == VK_NULL_HANDLE) {
		return false;
	}

	buffer->blend.image_view = create_view(dev, buffer->blend.image, blend_format);
	return buffer->blend.image_view != VK_NULL_HANDLE;
}

static bool bind_blend_descriptor(struct nx_vk_render_buffer *buffer) {
	struct nx_vk_renderer *renderer = buffer->renderer;
	buffer->blend.ds_pool = vulkan_alloc_blend_ds(renderer, &buffer->blend.ds);
	if (!buffer->blend.ds_pool) {
		nx_log(NX_ERROR, "failed to allocate descriptor");
		return false;
	}

	VkDescriptorImageInfo ds_attach_info = {
		.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		.imageView = buffer->blend.image_view,
		.sampler = VK_NULL_HANDLE,
	};
	VkWriteDescriptorSet ds_write = {
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,
		.dstSet = buffer->blend.ds,
		.dstBinding = 0,
		.pImageInfo = &ds_attach_info,
	};
	vkUpdateDescriptorSets(renderer->dev->dev, 1, &ds_write, 0, NULL);
	return true;
}

// Caller cleans up on failure.
bool vulkan_setup_two_pass_framebuffer(struct nx_vk_render_buffer *buffer,
		const struct nx_dmabuf_attributes *dmabuf) {
	struct nx_vk_renderer *renderer = buffer->renderer;
	struct nx_vk_render_target *target = &buffer->targets[NX_VK_PATHWAY_TWO_PASS];
	const struct nx_vk_format_props *fmt =
		vulkan_format_props_from_drm(renderer->dev, dmabuf->format);
	assert(fmt);

	target->image_view = create_view(renderer->dev->dev, buffer->image, fmt->format.vk);
	if (target->image_view == VK_NULL_HANDLE) {
		return false;
	}

	target->render_setup = vulkan_find_or_create_render_setup(renderer, &fmt->format,
		NX_VK_PATHWAY_TWO_PASS);
	if (!target->render_setup || !create_blend_image(buffer, dmabuf) ||
			!bind_blend_descriptor(buffer)) {
		return false;
	}

	struct attachment_list attachments = {
		.views = { buffer->blend.image_view, target->image_view },
		.len = 2,
	};
	return create_framebuffer(target, dmabuf, &attachments);
}

// Renders straight onto the buffer.
bool vulkan_setup_one_pass_framebuffer(struct nx_vk_render_buffer *buffer,
		const struct nx_dmabuf_attributes *dmabuf, enum nx_vk_pathway pathway) {
	struct nx_vk_renderer *renderer = buffer->renderer;
	struct nx_vk_render_target *target = &buffer->targets[pathway];
	const struct nx_vk_format_props *fmt =
		vulkan_format_props_from_drm(renderer->dev, dmabuf->format);
	assert(fmt);

	VkFormat vk_fmt = pathway == NX_VK_PATHWAY_SRGB ? fmt->format.vk_srgb : fmt->format.vk;
	assert(vk_fmt != VK_FORMAT_UNDEFINED);

	target->image_view = create_view(renderer->dev->dev, buffer->image, vk_fmt);
	if (target->image_view == VK_NULL_HANDLE) {
		return false;
	}

	target->render_setup = vulkan_find_or_create_render_setup(renderer, &fmt->format,
		pathway);
	struct attachment_list attachments = { .views = { target->image_view }, .len = 1 };
	return target->render_setup && create_framebuffer(target, dmabuf, &attachments);
}
