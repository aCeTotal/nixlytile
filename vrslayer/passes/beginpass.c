#include "passes.h"

#include <stdio.h>
#include <string.h>

#define BEGIN_MAX  4

/* Patched begin storage, never zeroed. */
struct begin {
	VkRenderPassBeginInfo patched;
	VkRenderPassAttachmentBeginInfo attachments;
	VkImageView views[ATTACH_MAX];
	union chain_node chain[BEGIN_MAX];
	VkCommandBuffer cmd;        /* marked when zoned */
};

/* Begin structs before attachments. */
static const struct chain_kind known[] = {
	{ VK_STRUCTURE_TYPE_DEVICE_GROUP_RENDER_PASS_BEGIN_INFO,
		sizeof(VkDeviceGroupRenderPassBeginInfo) },
	{ VK_STRUCTURE_TYPE_RENDER_PASS_SAMPLE_LOCATIONS_BEGIN_INFO_EXT,
		sizeof(VkRenderPassSampleLocationsBeginInfoEXT) },
	{ VK_STRUCTURE_TYPE_RENDER_PASS_TRANSFORM_BEGIN_INFO_QCOM,
		sizeof(VkRenderPassTransformBeginInfoQCOM) },
};

/* Ours replaces old; predecessors copied. */
static const VkRenderPassBeginInfo *
splice(const VkRenderPassBeginInfo *in, struct begin *b, const void *old)
{
	const VkBaseInStructure *s;
	void *next = &b->attachments;
	size_t size;
	int n = 0;

	for (s = in->pNext; s != old && n < BEGIN_MAX
			&& (size = chain_size(known, LENGTH(known), s->sType)); s = s->pNext)
		memcpy(&b->chain[n++], s, size);
	if (s != old)
		return in;
	while (n--) {
		b->chain[n].base.pNext = next;
		next = &b->chain[n];
	}
	b->patched.pNext = next;
	return &b->patched;
}

/* Imageless: rate view joins attachments. */
static const VkRenderPassBeginInfo *
attach(const VkRenderPassBeginInfo *in, struct begin *b, VkImageView view)
{
	const VkRenderPassAttachmentBeginInfo *old = (const void *)
		chain_find(in->pNext, VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO);
	uint32_t n;

	if (!old || old->attachmentCount >= ATTACH_MAX)
		return in;
	n = old->attachmentCount;
	if (n)
		memcpy(b->views, old->pAttachments, n * sizeof(*b->views));
	b->views[n] = view;
	b->attachments = *old;
	b->attachments.attachmentCount = n + 1;
	b->attachments.pAttachments = b->views;
	return splice(in, b, old);
}

/* Full scene passes: zone map. */
static VkImageView
choose_view(struct device *dev, const VkRenderPassBeginInfo *in,
	const struct framebuffer *f)
{
	VkExtent2D size = { f->create.width, f->create.height };
	const VkRect2D *area = &in->renderArea;
	VkImageView neutral = dev->passes->neutral.image.view, view;

	if (!fovea_active(dev) || atomic_load(&dev->unsafe)
			|| area->offset.x || area->offset.y || area->extent.width != size.width
			|| area->extent.height != size.height || f->create.layers != 1
			|| !scene_shaped(dev, size))
		return neutral;
	view = rate_view(dev, size, neutral);
	return view ? view : neutral;
}

static void
warn_untracked(void)
{
	static _Atomic int warned;

	if (!atomic_exchange(&warned, 1))
		fprintf(stderr, "nixly-vrs: untracked framebuffer, pass left native\n");
}

/* Always twin: pipelines target it. */
static const VkRenderPassBeginInfo *
patch(struct device *dev, const VkRenderPassBeginInfo *in, struct begin *b)
{
	const struct pass *p = pass_of(dev, in->renderPass);
	struct framebuffer *f = p ? framebuffer_of(dev, in->framebuffer) : NULL;
	struct twin twin;

	if (p && !f)
		warn_untracked();
	if (!f) {
		drawrate_plain(dev, b->cmd);
		return in;
	}
	twin = framebuffer_twin(dev, f,
		(struct twin_spec){ p->twin, choose_view(dev, in, f) });
	if (twin.view == dev->passes->neutral.image.view)
		drawrate_plain(dev, b->cmd);
	else
		drawrate_area(dev, b->cmd, in->renderArea.extent);
	b->patched = *in;
	b->patched.renderPass = p->twin;
	b->patched.framebuffer = twin.framebuffer;
	return f->views ? &b->patched : attach(in, b, twin.view);
}

VKAPI_ATTR void VKAPI_CALL
pass_begin(VkCommandBuffer cmd, const VkRenderPassBeginInfo *begin,
	VkSubpassContents contents)
{
	struct device *dev = device_of(cmd);
	struct begin b;

	b.cmd = cmd;
	dev->vk.CmdBeginRenderPass(cmd, patch(dev, begin, &b), contents);
}

VKAPI_ATTR void VKAPI_CALL
pass_begin2(VkCommandBuffer cmd, const VkRenderPassBeginInfo *begin,
	const VkSubpassBeginInfo *subpass)
{
	struct device *dev = device_of(cmd);
	struct begin b;

	b.cmd = cmd;
	dev->vk.CmdBeginRenderPass2(cmd, patch(dev, begin, &b), subpass);
}

/* Secondaries inherit the twin pass. */
const VkCommandBufferBeginInfo *
pass_inherit(struct device *dev, const VkCommandBufferBeginInfo *begin,
	struct inherited *scratch)
{
	const struct pass *p;

	if (!(begin->flags & VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT)
			|| !begin->pInheritanceInfo)
		return begin;
	p = pass_of(dev, begin->pInheritanceInfo->renderPass);
	if (!p)
		return begin;
	scratch->inheritance = *begin->pInheritanceInfo;
	scratch->inheritance.renderPass = p->twin;
	scratch->inheritance.framebuffer = VK_NULL_HANDLE;
	scratch->begin = *begin;
	scratch->begin.pInheritanceInfo = &scratch->inheritance;
	return &scratch->begin;
}
