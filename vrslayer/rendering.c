#include "layer.h"

#include <math.h>

#define MIN_SCALE    0.4f
#define ASPECT_SLACK 0.03f

static int
has_color(const VkRenderingInfo *info)
{
	uint32_t i;

	for (i = 0; i < info->colorAttachmentCount; i++)
		if (info->pColorAttachments[i].imageView != VK_NULL_HANDLE)
			return 1;
	return 0;
}

static int
screen_shaped(const struct device *dev, VkExtent2D extent)
{
	uint64_t swap = atomic_load(&dev->swap_extent);
	float sw = (float)(swap >> 32), sh = (float)(swap & UINT32_MAX);
	float w = (float)extent.width, h = (float)extent.height;

	if (!swap || w > sw || h > sh || w < MIN_SCALE * sw || h < MIN_SCALE * sh)
		return 0;
	return fabsf(w * sh - h * sw) <= ASPECT_SLACK * w * sh;
}

static int
app_sets_rate(const void *chain)
{
	const VkBaseInStructure *s;

	for (s = chain; s; s = s->pNext)
		if (s->sType == VK_STRUCTURE_TYPE_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR
				|| s->sType == VK_STRUCTURE_TYPE_RENDERING_FRAGMENT_DENSITY_MAP_ATTACHMENT_INFO_EXT)
			return 1;
	return 0;
}

/* Scene passes: depth, color, screen-shaped. */
static VkImageView
pick_view(struct device *dev, const VkRenderingInfo *info)
{
	VkImageView view;

	if (!dev->vrs || atomic_load(&dev->unsafe))
		return VK_NULL_HANDLE;
	if (info->flags & (VK_RENDERING_SUSPENDING_BIT | VK_RENDERING_RESUMING_BIT)
			|| info->viewMask || info->renderArea.offset.x
			|| info->renderArea.offset.y)
		return VK_NULL_HANDLE;
	if (!info->pDepthAttachment || !info->pDepthAttachment->imageView
			|| !has_color(info) || app_sets_rate(info->pNext)
			|| !screen_shaped(dev, info->renderArea.extent))
		return VK_NULL_HANDLE;
	view = rate_view(dev, info->renderArea.extent);
	return atomic_load(&dev->level) ? view : VK_NULL_HANDLE;
}

VKAPI_ATTR void VKAPI_CALL
rendering_begin(VkCommandBuffer cmd, const VkRenderingInfo *info)
{
	struct device *dev = device_of(cmd);
	VkImageView view = pick_view(dev, info);
	VkRenderingFragmentShadingRateAttachmentInfoKHR rate;
	VkRenderingInfo patched;

	if (!view) {
		dev->vk.CmdBeginRendering(cmd, info);
		return;
	}
	rate = (VkRenderingFragmentShadingRateAttachmentInfoKHR){
		.sType = VK_STRUCTURE_TYPE_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR,
		.pNext = info->pNext,
		.imageView = view,
		.imageLayout = VK_IMAGE_LAYOUT_GENERAL,
		.shadingRateAttachmentTexelSize = dev->texel,
	};
	patched = *info;
	patched.pNext = &rate;
	dev->vk.CmdBeginRendering(cmd, &patched);
}
