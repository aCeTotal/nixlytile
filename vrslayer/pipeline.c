#include "layer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FSR_BIT    VK_PIPELINE_CREATE_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR
#define FSR_BIT2   VK_PIPELINE_CREATE_2_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR
#define LENGTH(a)  (sizeof(a) / sizeof((a)[0]))

/* Structs copied when preceding flags2. */
static const struct {
	VkStructureType type;
	size_t size;
} known[] = {
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

static size_t
node_size(VkStructureType type)
{
	size_t i;

	for (i = 0; i < LENGTH(known); i++)
		if (known[i].type == type)
			return known[i].size;
	return 0;
}

/* Copies chain up to flags2. */
static void *
copy_to_flags2(const void *chain, struct patch *p)
{
	const VkBaseInStructure *s;
	VkBaseOutStructure *head = NULL, *prev = NULL, *node;
	size_t size;
	int n = 0;

	for (s = chain; s && n < CHAIN_MAX; s = s->pNext) {
		size = node_size(s->sType);
		if (!size || size > NODE_BYTES)
			return NULL;
		node = &p->chain[n++].base;
		memcpy(node, s, size);
		*(prev ? &prev->pNext : &head) = node;
		prev = node;
		if (s->sType != VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO)
			continue;
		((VkPipelineCreateFlags2CreateInfo *)node)->flags |= FSR_BIT2;
		return head;
	}
	return NULL;
}

static int
set_flag(const VkGraphicsPipelineCreateInfo *in,
	VkGraphicsPipelineCreateInfo *out, struct patch *p)
{
	if (!chain_find(in->pNext, VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO)) {
		out->flags |= FSR_BIT;
		return 1;
	}
	out->pNext = copy_to_flags2(in->pNext, p);
	return out->pNext != NULL;
}

/* Attachment flag, then per-draw rate. */
static int
patch_one(const VkGraphicsPipelineCreateInfo *in,
	VkGraphicsPipelineCreateInfo *out, struct patch *p)
{
	int flagged;

	*out = *in;
	flagged = in->renderPass != VK_NULL_HANDLE || set_flag(in, out, p);
	if (!flagged)
		*out = *in;
	drawrate_patch(in, out, p);
	return flagged;
}

static void
mark_unsafe(struct device *dev)
{
	if (!atomic_exchange(&dev->unsafe, 1))
		fprintf(stderr, "nixly-vrs: unpatchable pipeline, VRS off\n");
}

VKAPI_ATTR VkResult VKAPI_CALL
pipeline_create_graphics(VkDevice device, VkPipelineCache cache, uint32_t count,
	const VkGraphicsPipelineCreateInfo *infos,
	const VkAllocationCallbacks *alloc, VkPipeline *out)
{
	struct device *dev = device_of(device);
	VkGraphicsPipelineCreateInfo *copies;
	struct patch *patches;
	VkResult res;
	uint32_t i;

	if (!dev->vrs)
		return dev->vk.CreateGraphicsPipelines(device, cache, count, infos,
			alloc, out);
	copies = malloc(count * sizeof(*copies));
	patches = malloc(count * sizeof(*patches));
	if (!copies || !patches) {
		free(copies);
		free(patches);
		mark_unsafe(dev);
		return dev->vk.CreateGraphicsPipelines(device, cache, count, infos,
			alloc, out);
	}
	for (i = 0; i < count; i++) {
		if (!patch_one(&infos[i], &copies[i], &patches[i]))
			mark_unsafe(dev);
		if (patches[i].bind_rate)
			atomic_store(&dev->bind_rate, 1);
	}
	res = dev->vk.CreateGraphicsPipelines(device, cache, count, copies, alloc, out);
	free(copies);
	free(patches);
	return res;
}
