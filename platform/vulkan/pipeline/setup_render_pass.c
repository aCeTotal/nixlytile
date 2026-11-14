#include <assert.h>
#include <vulkan/vulkan.h>
#include "platform/log/log.h"
#include "platform/vulkan/pipeline/render_setup.h"
#include "platform/vulkan/renderer.h"

// Uploads finish first.
static const VkSubpassDependency external_to_first = {
	.srcSubpass = VK_SUBPASS_EXTERNAL,
	.srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT |
		VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT |
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
	.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT |
		VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
	.dstSubpass = 0,
	.dstStageMask = VK_PIPELINE_STAGE_VERTEX_INPUT_BIT |
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
	.dstAccessMask = VK_ACCESS_UNIFORM_READ_BIT |
		VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT |
		VK_ACCESS_SHADER_READ_BIT,
};

static VkSubpassDependency last_to_external(uint32_t subpass) {
	return (VkSubpassDependency){
		.srcSubpass = subpass,
		.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		.dstSubpass = VK_SUBPASS_EXTERNAL,
		.dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT |
			VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
		.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT |
			VK_ACCESS_MEMORY_READ_BIT,
	};
}

static VkAttachmentDescription color_attachment(VkFormat format, VkImageLayout initial,
		VkImageLayout final) {
	return (VkAttachmentDescription){
		.format = format,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
		.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
		.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
		.initialLayout = initial,
		.finalLayout = final,
	};
}

// Blend in 16F, then output.
bool vulkan_create_two_pass_render_pass(struct nx_vk_render_format_setup *setup) {
	VkDevice dev = setup->renderer->dev->dev;
	VkAttachmentDescription attachments[] = {
		color_attachment(VK_FORMAT_R16G16B16A16_SFLOAT,
			VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
		color_attachment(setup->render_format->vk,
			VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL),
	};

	VkAttachmentReference blend_write_ref = {
		.attachment = 0u,
		.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	};
	VkAttachmentReference blend_read_ref = {
		.attachment = 0u,
		.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	};
	VkAttachmentReference color_ref = {
		.attachment = 1u,
		.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	};
	VkSubpassDescription subpasses[] = {
		{
			.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
			.colorAttachmentCount = 1,
			.pColorAttachments = &blend_write_ref,
		},
		{
			.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
			.inputAttachmentCount = 1,
			.pInputAttachments = &blend_read_ref,
			.colorAttachmentCount = 1,
			.pColorAttachments = &color_ref,
		},
	};

	VkSubpassDependency deps[] = {
		external_to_first,
		{
			.srcSubpass = 0,
			.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
			.dstSubpass = 1,
			.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
			.dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
			.dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT,
		},
		last_to_external(1),
	};

	VkRenderPassCreateInfo rp_info = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
		.attachmentCount = sizeof(attachments) / sizeof(attachments[0]),
		.pAttachments = attachments,
		.subpassCount = sizeof(subpasses) / sizeof(subpasses[0]),
		.pSubpasses = subpasses,
		.dependencyCount = sizeof(deps) / sizeof(deps[0]),
		.pDependencies = deps,
	};
	VkResult res = vkCreateRenderPass(dev, &rp_info, NULL, &setup->render_pass);
	if (res != VK_SUCCESS) {
		nx_vk_error("Failed to create 2-step render pass", res);
		return false;
	}

	// Clearing variant for first use.
	attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	res = vkCreateRenderPass(dev, &rp_info, NULL, &setup->render_pass_clear);
	if (res != VK_SUCCESS) {
		nx_vk_error("Failed to create 2-step clear render pass", res);
		return false;
	}
	return true;
}

bool vulkan_create_one_pass_render_pass(struct nx_vk_render_format_setup *setup) {
	VkFormat format = setup->render_format->vk;
	if (setup->pathway == NX_VK_PATHWAY_SRGB) {
		assert(setup->render_format->vk_srgb);
		format = setup->render_format->vk_srgb;
	}
	VkAttachmentDescription attachment = color_attachment(format,
		VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);

	VkAttachmentReference color_ref = {
		.attachment = 0u,
		.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	};
	VkSubpassDescription subpass = {
		.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
		.colorAttachmentCount = 1,
		.pColorAttachments = &color_ref,
	};
	VkSubpassDependency deps[] = {
		external_to_first,
		last_to_external(0),
	};

	VkRenderPassCreateInfo rp_info = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
		.attachmentCount = 1,
		.pAttachments = &attachment,
		.subpassCount = 1,
		.pSubpasses = &subpass,
		.dependencyCount = sizeof(deps) / sizeof(deps[0]),
		.pDependencies = deps,
	};
	VkResult res = vkCreateRenderPass(setup->renderer->dev->dev, &rp_info, NULL,
		&setup->render_pass);
	if (res != VK_SUCCESS) {
		nx_vk_error("Failed to create render pass", res);
		return false;
	}
	return true;
}
