#include "passes.h"

#include <stdlib.h>
#include <string.h>

/* Storage behind one twin create. */
struct twin_build {
	VkAttachmentDescription2 attachments[ATTACH_MAX];
	VkSubpassDescription2 subpasses[SUBPASS_MAX];
	VkFragmentShadingRateAttachmentInfoKHR rates[SUBPASS_MAX];
	VkAttachmentReference2 ref;
};

static const VkAttachmentDescription2 rate_attachment = {
	.sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2,
	.format = RATE_FORMAT,
	.samples = VK_SAMPLE_COUNT_1_BIT,
	.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
	.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
	.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
	.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
	.initialLayout = VK_IMAGE_LAYOUT_GENERAL,
	.finalLayout = VK_IMAGE_LAYOUT_GENERAL,
};

static int
has_depth(const VkRenderPassCreateInfo2 *in, const VkSubpassDescription2 *s)
{
	const VkAttachmentReference2 *d = s->pDepthStencilAttachment;

	return d && d->attachment < in->attachmentCount
		&& format_aspects(in->pAttachments[d->attachment].format)
			& VK_IMAGE_ASPECT_DEPTH_BIT;
}

static int
has_color(const VkSubpassDescription2 *s)
{
	uint32_t i = 0;

	while (i < s->colorAttachmentCount
			&& s->pColorAttachments[i].attachment == VK_ATTACHMENT_UNUSED)
		i++;
	return i < s->colorAttachmentCount;
}

static int
scene_subpass(const VkRenderPassCreateInfo2 *in, const VkSubpassDescription2 *s)
{
	return s->pipelineBindPoint == VK_PIPELINE_BIND_POINT_GRAPHICS
		&& has_depth(in, s) && has_color(s);
}

/* Scene subpasses: depth plus color. */
static uint32_t
depth_subpasses(const VkRenderPassCreateInfo2 *in)
{
	uint32_t i, mask = 0;

	for (i = 0; i < in->subpassCount; i++)
		mask |= (uint32_t)scene_subpass(in, &in->pSubpasses[i]) << i;
	return mask;
}

static int
plain_subpass(const VkSubpassDescription2 *s)
{
	return !s->viewMask && !chain_find(s->pNext,
		VK_STRUCTURE_TYPE_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR);
}

/* Untwinnable: multiview, density, app rates. */
static int
twinnable(const VkRenderPassCreateInfo2 *in)
{
	uint32_t i = 0;

	if (in->subpassCount > SUBPASS_MAX || in->attachmentCount >= ATTACH_MAX
			|| in->correlatedViewMaskCount || chain_find(in->pNext,
				VK_STRUCTURE_TYPE_RENDER_PASS_FRAGMENT_DENSITY_MAP_CREATE_INFO_EXT))
		return 0;
	while (i < in->subpassCount && plain_subpass(&in->pSubpasses[i]))
		i++;
	return i == in->subpassCount;
}

/* Same pass plus rate attachment. */
static VkRenderPass
create_twin(struct device *dev, const VkRenderPassCreateInfo2 *in, uint32_t depth)
{
	struct twin_build b;
	VkRenderPassCreateInfo2 create = *in;
	VkRenderPass twin = VK_NULL_HANDLE;
	uint32_t i, n = in->attachmentCount;

	if (n)
		memcpy(b.attachments, in->pAttachments, n * sizeof(*b.attachments));
	b.attachments[n] = rate_attachment;
	b.ref = (VkAttachmentReference2){
		.sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2,
		.attachment = n,
		.layout = VK_IMAGE_LAYOUT_GENERAL,
	};
	for (i = 0; i < in->subpassCount; i++) {
		b.subpasses[i] = in->pSubpasses[i];
		b.rates[i] = (VkFragmentShadingRateAttachmentInfoKHR){
			.sType = VK_STRUCTURE_TYPE_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR,
			.pNext = in->pSubpasses[i].pNext,
			.pFragmentShadingRateAttachment = &b.ref,
			.shadingRateAttachmentTexelSize = dev->texel,
		};
		b.subpasses[i].pNext = depth >> i & 1 ? &b.rates[i]
			: in->pSubpasses[i].pNext;
	}
	create.attachmentCount = n + 1;
	create.pAttachments = b.attachments;
	create.pSubpasses = b.subpasses;
	if (dev->vk.CreateRenderPass2(dev->handle, &create, NULL, &twin) != VK_SUCCESS)
		return VK_NULL_HANDLE;
	return twin;
}

static void
forget_pass(struct device *dev, struct pass *p)
{
	dev->vk.DestroyRenderPass(dev->handle, p->twin, NULL);
	free(p);
}

static void
remember(struct device *dev, VkRenderPass handle, const VkRenderPassCreateInfo2 *in)
{
	uint32_t depth = dev->passes->neutral.image.view && twinnable(in) ? depth_subpasses(in) : 0;
	struct pass *p = depth ? malloc(sizeof(*p)) : NULL;

	if (!p)
		return;
	*p = (struct pass){ create_twin(dev, in, depth), depth };
	if (!p->twin || !handlemap_put(&dev->passes->renderpasses, (uint64_t)handle,
			(uint64_t)(uintptr_t)p))
		forget_pass(dev, p);
}

const struct pass *
pass_of(struct device *dev, VkRenderPass handle)
{
	if (!handle)
		return NULL;
	return (const struct pass *)(uintptr_t)
		handlemap_get(&dev->passes->renderpasses, (uint64_t)handle);
}

int
pass_subpass_depth(const struct pass *p, uint32_t subpass)
{
	return p && subpass < SUBPASS_MAX && (p->depth >> subpass & 1);
}

void
pass_drop(uint64_t value, void *dev)
{
	forget_pass(dev, (struct pass *)(uintptr_t)value);
}

VKAPI_ATTR VkResult VKAPI_CALL
renderpass_create(VkDevice device, const VkRenderPassCreateInfo *create,
	const VkAllocationCallbacks *alloc, VkRenderPass *out)
{
	struct device *dev = device_of(device);
	VkResult created = dev->vk.CreateRenderPass(device, create, alloc, out);
	VkRenderPassCreateInfo2 *v2 = created == VK_SUCCESS
		? pass_convert(create) : NULL;

	if (!v2)
		return created;
	remember(dev, *out, v2);
	pass_convert_free(v2);
	return created;
}

VKAPI_ATTR VkResult VKAPI_CALL
renderpass_create2(VkDevice device, const VkRenderPassCreateInfo2 *create,
	const VkAllocationCallbacks *alloc, VkRenderPass *out)
{
	struct device *dev = device_of(device);
	VkResult created = dev->vk.CreateRenderPass2(device, create, alloc, out);

	if (created == VK_SUCCESS)
		remember(dev, *out, create);
	return created;
}

VKAPI_ATTR void VKAPI_CALL
renderpass_destroy(VkDevice device, VkRenderPass handle,
	const VkAllocationCallbacks *alloc)
{
	struct device *dev = device_of(device);
	struct pass *p = handle ? (struct pass *)(uintptr_t)
		handlemap_take(&dev->passes->renderpasses, (uint64_t)handle) : NULL;

	if (p)
		forget_pass(dev, p);
	dev->vk.DestroyRenderPass(device, handle, alloc);
}
