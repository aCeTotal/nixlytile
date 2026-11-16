#include "passes.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Patched copies per create. */
struct batch {
	const VkGraphicsPipelineCreateInfo *in;
	VkGraphicsPipelineCreateInfo *copies;
	struct patch *patches;
	VkPipeline *out;
};

/* Structs copied when preceding flags2. */
static const struct chain_kind known[] = {
	{ VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO,
		sizeof(VkPipelineCreateFlags2CreateInfo) },
	{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
		sizeof(VkPipelineRenderingCreateInfo) },
	{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT,
		sizeof(VkGraphicsPipelineLibraryCreateInfoEXT) },
	{ VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR,
		sizeof(VkPipelineLibraryCreateInfoKHR) },
	{ VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO,
		sizeof(VkPipelineCreationFeedbackCreateInfo) },
	{ VK_STRUCTURE_TYPE_PIPELINE_ROBUSTNESS_CREATE_INFO,
		sizeof(VkPipelineRobustnessCreateInfo) },
	{ VK_STRUCTURE_TYPE_PIPELINE_DISCARD_RECTANGLE_STATE_CREATE_INFO_EXT,
		sizeof(VkPipelineDiscardRectangleStateCreateInfoEXT) },
	{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_LOCATION_INFO,
		sizeof(VkRenderingAttachmentLocationInfo) },
	{ VK_STRUCTURE_TYPE_RENDERING_INPUT_ATTACHMENT_INDEX_INFO,
		sizeof(VkRenderingInputAttachmentIndexInfo) },
	{ VK_STRUCTURE_TYPE_ATTACHMENT_SAMPLE_COUNT_INFO_AMD,
		sizeof(VkAttachmentSampleCountInfoAMD) },
	{ VK_STRUCTURE_TYPE_PIPELINE_COMPILER_CONTROL_CREATE_INFO_AMD,
		sizeof(VkPipelineCompilerControlCreateInfoAMD) },
	{ VK_STRUCTURE_TYPE_PIPELINE_REPRESENTATIVE_FRAGMENT_TEST_STATE_CREATE_INFO_NV,
		sizeof(VkPipelineRepresentativeFragmentTestStateCreateInfoNV) },
	{ VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_STATE_CREATE_INFO_KHR,
		sizeof(VkPipelineFragmentShadingRateStateCreateInfoKHR) },
	{ VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_ENUM_STATE_CREATE_INFO_NV,
		sizeof(VkPipelineFragmentShadingRateEnumStateCreateInfoNV) },
	{ VK_STRUCTURE_TYPE_MULTIVIEW_PER_VIEW_ATTRIBUTES_INFO_NVX,
		sizeof(VkMultiviewPerViewAttributesInfoNVX) },
	{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_SHADER_GROUPS_CREATE_INFO_NV,
		sizeof(VkGraphicsPipelineShaderGroupsCreateInfoNV) },
	{ VK_STRUCTURE_TYPE_PIPELINE_BINARY_INFO_KHR,
		sizeof(VkPipelineBinaryInfoKHR) },
};

static int
is_flags2(const VkBaseOutStructure *node)
{
	return node && node->sType == VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO;
}

/* FSR-flagged copy, else NULL. */
static void *
copy_to_flags2(const void *chain, struct patch *p)
{
	const VkBaseInStructure *s;
	VkBaseOutStructure *head = NULL, *prev = NULL, *node;
	size_t size;
	int n = 0;

	for (s = chain; s && n < CHAIN_MAX && !is_flags2(prev)
			&& (size = chain_size(known, LENGTH(known), s->sType)); s = s->pNext) {
		node = &p->chain[n++].base;
		memcpy(node, s, size);
		*(prev ? &prev->pNext : &head) = node;
		prev = node;
	}
	if (!is_flags2(prev))
		return NULL;
	((VkPipelineCreateFlags2CreateInfo *)prev)->flags |= FSR_BIT2;
	return head;
}

static void
mark_unsafe(struct device *dev)
{
	if (!atomic_exchange(&dev->unsafe, 1))
		fprintf(stderr, "nixly-vrs: unpatchable pipeline, VRS off\n");
}

/* Attachment flag, else VRS off. */
static VkGraphicsPipelineCreateInfo
flagged(struct device *dev, const VkGraphicsPipelineCreateInfo *in, struct patch *p)
{
	VkGraphicsPipelineCreateInfo out = *in;

	if (in->renderPass)
		return out;
	if (!chain_find(in->pNext, VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO)) {
		out.flags |= FSR_BIT;
		return out;
	}
	out.pNext = copy_to_flags2(in->pNext, p);
	if (!out.pNext) {
		mark_unsafe(dev);
		return *in;
	}
	return out;
}

/* Pipelines target twin passes. */
static void
prepare(struct device *dev, const VkGraphicsPipelineCreateInfo *in,
	struct patch *p)
{
	const struct pass *rp = pass_of(dev, in->renderPass);

	p->facts = pipestate_facts(dev, in);
	p->twin = rp ? rp->twin : VK_NULL_HANDLE;
}

/* One create: facts, flag, rate. */
static void
patch_at(struct device *dev, const struct batch *b, uint32_t i)
{
	const VkGraphicsPipelineCreateInfo *in = &b->in[i];
	struct patch *p = &b->patches[i];

	prepare(dev, in, p);
	b->copies[i] = flagged(dev, in, p);
	pipestate_patch(in, &b->copies[i], p);
	if (p->facts & FACT_DYNAMIC_RATE)
		atomic_store(&dev->bind_rate, 1);
}

/* Lost facts: binds stay native. */
static void
keep_facts(struct device *dev, const struct batch *b, uint32_t i)
{
	if (!b->out[i] || !pipestate_kept(&b->in[i], &b->patches[i]))
		return;
	if (!handlemap_put(&dev->passes->pipelines, (uint64_t)b->out[i],
			b->patches[i].facts))
		atomic_store(&dev->passes->blind, 1);
}

VKAPI_ATTR VkResult VKAPI_CALL
pipeline_create_graphics(VkDevice device, VkPipelineCache cache, uint32_t count,
	const VkGraphicsPipelineCreateInfo *creates,
	const VkAllocationCallbacks *alloc, VkPipeline *out)
{
	struct device *dev = device_of(device);
	struct batch b = { .in = creates, .out = out };
	VkResult created;
	uint32_t i;

	b.copies = malloc(count * sizeof(*b.copies));
	b.patches = malloc(count * sizeof(*b.patches));
	if (!b.copies || !b.patches) {
		free(b.copies);
		free(b.patches);
		mark_unsafe(dev);
		return dev->vk.CreateGraphicsPipelines(device, cache, count, creates,
			alloc, out);
	}
	for (i = 0; i < count; i++)
		patch_at(dev, &b, i);
	created = dev->vk.CreateGraphicsPipelines(device, cache, count, b.copies, alloc, out);
	for (i = 0; i < count; i++)
		keep_facts(dev, &b, i);
	free(b.copies);
	free(b.patches);
	return created;
}

VKAPI_ATTR void VKAPI_CALL
pipeline_destroy(VkDevice device, VkPipeline pipeline,
	const VkAllocationCallbacks *alloc)
{
	struct device *dev = device_of(device);

	if (pipeline)
		handlemap_take(&dev->passes->pipelines, (uint64_t)pipeline);
	dev->vk.DestroyPipeline(device, pipeline, alloc);
}
