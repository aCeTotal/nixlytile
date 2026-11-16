#include "passes.h"

#include <stdlib.h>

/* Converted pass and its arrays. */
struct block {
	VkRenderPassCreateInfo2 pass;
	VkAttachmentDescription2 *attachments;
	VkSubpassDescription2 *subpasses;
	VkSubpassDependency2 *dependencies;
	VkAttachmentReference2 *refs;
};

/* Source pass and unused refs. */
struct convert {
	const VkRenderPassCreateInfo *in;
	const VkRenderPassInputAttachmentAspectCreateInfo *aspects;
	VkAttachmentReference2 *next;
};

VkImageAspectFlags
format_aspects(VkFormat format)
{
	switch (format) {
	case VK_FORMAT_D16_UNORM:
	case VK_FORMAT_X8_D24_UNORM_PACK32:
	case VK_FORMAT_D32_SFLOAT:
		return VK_IMAGE_ASPECT_DEPTH_BIT;
	case VK_FORMAT_S8_UINT:
		return VK_IMAGE_ASPECT_STENCIL_BIT;
	case VK_FORMAT_D16_UNORM_S8_UINT:
	case VK_FORMAT_D24_UNORM_S8_UINT:
	case VK_FORMAT_D32_SFLOAT_S8_UINT:
		return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
	default:
		return VK_IMAGE_ASPECT_COLOR_BIT;
	}
}

static uint32_t
ref_count(const VkRenderPassCreateInfo *in)
{
	const VkSubpassDescription *s;
	uint32_t i, n = 0;

	for (i = 0; i < in->subpassCount; i++) {
		s = &in->pSubpasses[i];
		n += s->inputAttachmentCount + s->colorAttachmentCount
			+ (s->pResolveAttachments ? s->colorAttachmentCount : 0)
			+ (s->pDepthStencilAttachment != NULL);
	}
	return n;
}

static struct block *
allocate(const VkRenderPassCreateInfo *in)
{
	struct block *b;
	size_t at = in->attachmentCount * sizeof(*b->attachments);
	size_t sub = in->subpassCount * sizeof(*b->subpasses);
	size_t dep = in->dependencyCount * sizeof(*b->dependencies);
	char *base;

	b = malloc(sizeof(*b) + at + sub + dep + ref_count(in) * sizeof(*b->refs));
	if (!b)
		return NULL;
	base = (char *)(b + 1);
	b->attachments = (void *)base;
	b->subpasses = (void *)(base + at);
	b->dependencies = (void *)(base + at + sub);
	b->refs = (void *)(base + at + sub + dep);
	return b;
}

static int
refers_to(const VkInputAttachmentAspectReference *r, uint32_t subpass, uint32_t index)
{
	return r->subpass == subpass && r->inputAttachmentIndex == index;
}

/* Explicit aspect, else format's. */
static VkImageAspectFlags
input_aspects(const struct convert *c, uint32_t subpass, uint32_t index)
{
	const VkRenderPassInputAttachmentAspectCreateInfo *a = c->aspects;
	uint32_t i = 0, n = a ? a->aspectReferenceCount : 0;
	uint32_t att = c->in->pSubpasses[subpass].pInputAttachments[index].attachment;

	while (i < n && !refers_to(&a->pAspectReferences[i], subpass, index))
		i++;
	if (i < n)
		return a->pAspectReferences[i].aspectMask;
	return att == VK_ATTACHMENT_UNUSED ? 0 : format_aspects(c->in->pAttachments[att].format);
}

static VkAttachmentReference2 *
copy_refs(struct convert *c, const VkAttachmentReference *refs, uint32_t n)
{
	VkAttachmentReference2 *out = c->next;
	uint32_t i;

	for (i = 0; i < n; i++)
		out[i] = (VkAttachmentReference2){
			.sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2,
			.attachment = refs[i].attachment,
			.layout = refs[i].layout,
		};
	c->next = out + n;
	return refs ? out : NULL;
}

static void
convert_subpass(struct convert *c, uint32_t index, VkSubpassDescription2 *out)
{
	const VkSubpassDescription *s = &c->in->pSubpasses[index];
	VkAttachmentReference2 *inputs = copy_refs(c, s->pInputAttachments,
		s->inputAttachmentCount);
	uint32_t i;

	for (i = 0; i < s->inputAttachmentCount; i++)
		inputs[i].aspectMask = input_aspects(c, index, i);
	*out = (VkSubpassDescription2){
		.sType = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2,
		.flags = s->flags,
		.pipelineBindPoint = s->pipelineBindPoint,
		.inputAttachmentCount = s->inputAttachmentCount,
		.pInputAttachments = inputs,
		.colorAttachmentCount = s->colorAttachmentCount,
		.pColorAttachments = copy_refs(c, s->pColorAttachments,
			s->colorAttachmentCount),
		.pResolveAttachments = copy_refs(c, s->pResolveAttachments,
			s->pResolveAttachments ? s->colorAttachmentCount : 0),
		.pDepthStencilAttachment = copy_refs(c, s->pDepthStencilAttachment,
			s->pDepthStencilAttachment != NULL),
		.preserveAttachmentCount = s->preserveAttachmentCount,
		.pPreserveAttachments = s->pPreserveAttachments,
	};
}

static void
convert_attachment(const VkAttachmentDescription *a, VkAttachmentDescription2 *out)
{
	*out = (VkAttachmentDescription2){
		.sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2,
		.flags = a->flags,
		.format = a->format,
		.samples = a->samples,
		.loadOp = a->loadOp,
		.storeOp = a->storeOp,
		.stencilLoadOp = a->stencilLoadOp,
		.stencilStoreOp = a->stencilStoreOp,
		.initialLayout = a->initialLayout,
		.finalLayout = a->finalLayout,
	};
}

static void
convert_dependency(const VkSubpassDependency *d, VkSubpassDependency2 *out)
{
	*out = (VkSubpassDependency2){
		.sType = VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2,
		.srcSubpass = d->srcSubpass,
		.dstSubpass = d->dstSubpass,
		.srcStageMask = d->srcStageMask,
		.dstStageMask = d->dstStageMask,
		.srcAccessMask = d->srcAccessMask,
		.dstAccessMask = d->dstAccessMask,
		.dependencyFlags = d->dependencyFlags,
	};
}

/* Only input-aspect pNext converts. */
VkRenderPassCreateInfo2 *
pass_convert(const VkRenderPassCreateInfo *in)
{
	const VkRenderPassInputAttachmentAspectCreateInfo *aspects = (const void *)
		chain_find(in->pNext, VK_STRUCTURE_TYPE_RENDER_PASS_INPUT_ATTACHMENT_ASPECT_CREATE_INFO);
	struct convert c = { in, aspects, NULL };
	struct block *b;
	uint32_t i;

	if (in->pNext && (in->pNext != (const void *)aspects || aspects->pNext))
		return NULL;
	b = allocate(in);
	if (!b)
		return NULL;
	c.next = b->refs;
	for (i = 0; i < in->attachmentCount; i++)
		convert_attachment(&in->pAttachments[i], &b->attachments[i]);
	for (i = 0; i < in->subpassCount; i++)
		convert_subpass(&c, i, &b->subpasses[i]);
	for (i = 0; i < in->dependencyCount; i++)
		convert_dependency(&in->pDependencies[i], &b->dependencies[i]);
	b->pass = (VkRenderPassCreateInfo2){
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2,
		.flags = in->flags,
		.attachmentCount = in->attachmentCount,
		.pAttachments = b->attachments,
		.subpassCount = in->subpassCount,
		.pSubpasses = b->subpasses,
		.dependencyCount = in->dependencyCount,
		.pDependencies = b->dependencies,
	};
	return &b->pass;
}

/* Pass heads its block. */
void
pass_convert_free(VkRenderPassCreateInfo2 *pass)
{
	free(pass);
}
