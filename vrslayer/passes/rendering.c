#include "passes.h"

#include <math.h>

#define MIN_SCALE          0.3f
#define ASPECT_SLACK       0.03f
#define RATE_ATTACHMENT    VK_STRUCTURE_TYPE_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR
#define DENSITY_ATTACHMENT VK_STRUCTURE_TYPE_RENDERING_FRAGMENT_DENSITY_MAP_ATTACHMENT_INFO_EXT
#define INHERITANCE        VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO
#define SPLIT              (VK_RENDERING_SUSPENDING_BIT | VK_RENDERING_RESUMING_BIT)

static int
has_color(const VkRenderingInfo *rendering)
{
	uint32_t i = 0;

	while (i < rendering->colorAttachmentCount
			&& rendering->pColorAttachments[i].imageView == VK_NULL_HANDLE)
		i++;
	return i < rendering->colorAttachmentCount;
}

static int
has_color_format(const VkCommandBufferInheritanceRenderingInfo *r)
{
	uint32_t i = 0;

	while (i < r->colorAttachmentCount
			&& r->pColorAttachmentFormats[i] == VK_FORMAT_UNDEFINED)
		i++;
	return i < r->colorAttachmentCount;
}

/* Pass pick_view may zone. */
int
scene_inherited(const VkCommandBufferInheritanceInfo *in)
{
	const VkCommandBufferInheritanceRenderingInfo *r = (const void *)
		chain_find(in->pNext, INHERITANCE);

	return !in->renderPass && r && !(r->flags & SPLIT) && !r->viewMask
		&& r->depthAttachmentFormat != VK_FORMAT_UNDEFINED && has_color_format(r);
}

int
scene_shaped(const struct device *dev, VkExtent2D extent)
{
	VkExtent2D swap = extent_of(atomic_load(&dev->swap_extent));
	float sw = (float)swap.width, sh = (float)swap.height;
	float w = (float)extent.width, h = (float)extent.height;

	if (!swap.width || w > sw || h > sh || w < MIN_SCALE * sw || h < MIN_SCALE * sh)
		return 0;
	return fabsf(w * sh - h * sw) <= ASPECT_SLACK * w * sh;
}

static int
app_sets_rate(const void *chain)
{
	const VkBaseInStructure *s = chain;

	while (s && s->sType != RATE_ATTACHMENT && s->sType != DENSITY_ATTACHMENT)
		s = s->pNext;
	return s != NULL;
}

/* Neutral until ready, native bare. */
static VkImageView
pick_view(struct device *dev, const VkRenderingInfo *rendering)
{
	if (atomic_load(&dev->unsafe) || !fovea_active(dev))
		return VK_NULL_HANDLE;
	if (rendering->flags & SPLIT || rendering->viewMask || rendering->layerCount > 1
			|| rendering->renderArea.offset.x || rendering->renderArea.offset.y)
		return VK_NULL_HANDLE;
	if (!rendering->pDepthAttachment || !rendering->pDepthAttachment->imageView
			|| !has_color(rendering) || app_sets_rate(rendering->pNext)
			|| !scene_shaped(dev, rendering->renderArea.extent))
		return VK_NULL_HANDLE;
	return rate_view(dev, rendering->renderArea.extent, dev->passes->neutral.image.view);
}

/* Rate set first: secondary-only contents. */
VKAPI_ATTR void VKAPI_CALL
rendering_begin(VkCommandBuffer cmd, const VkRenderingInfo *rendering)
{
	struct device *dev = device_of(cmd);
	VkImageView view = pick_view(dev, rendering);
	VkRenderingFragmentShadingRateAttachmentInfoKHR rate;
	VkRenderingInfo patched;

	if (!view) {
		drawrate_plain(dev, cmd);
		dev->vk.CmdBeginRendering(cmd, rendering);
		return;
	}
	if (view == dev->passes->neutral.image.view)
		drawrate_plain(dev, cmd);
	else
		drawrate_area(dev, cmd, rendering->renderArea.extent);
	rate = (VkRenderingFragmentShadingRateAttachmentInfoKHR){
		.sType = RATE_ATTACHMENT,
		.pNext = rendering->pNext,
		.imageView = view,
		.imageLayout = VK_IMAGE_LAYOUT_GENERAL,
		.shadingRateAttachmentTexelSize = dev->texel,
	};
	patched = *rendering;
	patched.pNext = &rate;
	dev->vk.CmdBeginRendering(cmd, &patched);
}
