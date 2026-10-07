#include "layer.h"

#include <string.h>

#define RECENT          4
#define UNKNOWN         (-1)
#define FSR_DYNAMIC     VK_DYNAMIC_STATE_FRAGMENT_SHADING_RATE_KHR
#define LIBRARY_BIT     VK_PIPELINE_CREATE_LIBRARY_BIT_KHR
#define LIBRARY_BIT2    VK_PIPELINE_CREATE_2_LIBRARY_BIT_KHR
#define FRAGMENT_SUBSET VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT
#define RATE_SUBSETS    (VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT \
	| FRAGMENT_SUBSET)

enum kind { KIND_COMPLETE, KIND_LIBRARY, KIND_LINKED };

/* Depth state per command buffer. */
struct recording {
	VkCommandBuffer cmd;
	signed char test;
	signed char compares;
};

static _Thread_local struct recording recent[RECENT];
static _Thread_local unsigned recent_next;

static enum kind
kind_of(const VkGraphicsPipelineCreateInfo *in)
{
	const VkPipelineCreateFlags2CreateInfo *f2 = (const void *)
		chain_find(in->pNext, VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO);
	const VkPipelineLibraryCreateInfoKHR *libs = (const void *)
		chain_find(in->pNext, VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR);

	if (f2 ? f2->flags & LIBRARY_BIT2 : in->flags & LIBRARY_BIT)
		return KIND_LIBRARY;
	return libs && libs->libraryCount ? KIND_LINKED : KIND_COMPLETE;
}

static VkGraphicsPipelineLibraryFlagsEXT
subsets(const VkGraphicsPipelineCreateInfo *in)
{
	const VkGraphicsPipelineLibraryCreateInfoEXT *l = (const void *)
		chain_find(in->pNext, VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT);

	return l ? l->flags : RATE_SUBSETS;
}

static int
is_dynamic(const VkGraphicsPipelineCreateInfo *in, VkDynamicState state)
{
	const VkPipelineDynamicStateCreateInfo *d = in->pDynamicState;
	uint32_t i;

	for (i = 0; d && i < d->dynamicStateCount; i++)
		if (d->pDynamicStates[i] == state)
			return 1;
	return 0;
}

static int
depth_dynamic(const VkGraphicsPipelineCreateInfo *in)
{
	return is_dynamic(in, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE)
		|| is_dynamic(in, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP);
}

static int
compares(VkCompareOp op)
{
	return op != VK_COMPARE_OP_ALWAYS && op != VK_COMPARE_OP_NEVER;
}

/* Depth-tested draws: scene, not UI. */
static int
depth_tested(const VkGraphicsPipelineCreateInfo *in)
{
	const VkPipelineRenderingCreateInfo *r = (const void *)
		chain_find(in->pNext, VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO);
	const VkPipelineDepthStencilStateCreateInfo *ds = in->pDepthStencilState;
	const VkPipelineRasterizationStateCreateInfo *rs = in->pRasterizationState;

	if (!r || r->depthAttachmentFormat == VK_FORMAT_UNDEFINED || !ds || !rs)
		return 0;
	if (rs->rasterizerDiscardEnable
			&& !is_dynamic(in, VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE))
		return 0;
	return ds->depthTestEnable && compares(ds->depthCompareOp);
}

static void
add_dynamic(const VkGraphicsPipelineCreateInfo *in,
	VkGraphicsPipelineCreateInfo *out, struct patch *p)
{
	const VkPipelineDynamicStateCreateInfo *d = in->pDynamicState;
	uint32_t n = d ? d->dynamicStateCount : 0;

	if (n >= DYNAMIC_MAX || is_dynamic(in, FSR_DYNAMIC))
		return;
	if (n)
		memcpy(p->states, d->pDynamicStates, n * sizeof(*p->states));
	p->states[n] = FSR_DYNAMIC;
	p->dynamic = d ? *d : (VkPipelineDynamicStateCreateInfo){
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
	};
	p->dynamic.dynamicStateCount = n + 1;
	p->dynamic.pDynamicStates = p->states;
	out->pDynamicState = &p->dynamic;
}

static void
add_static(const VkGraphicsPipelineCreateInfo *in,
	VkGraphicsPipelineCreateInfo *out, struct patch *p)
{
	p->fsr = (VkPipelineFragmentShadingRateStateCreateInfoKHR){
		.sType = VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_STATE_CREATE_INFO_KHR,
		.pNext = out->pNext,
		.fragmentSize = { 1, 1 },
		.combinerOps = {
			VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR,
			depth_tested(in) ? VK_FRAGMENT_SHADING_RATE_COMBINER_OP_REPLACE_KHR
				: VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR,
		},
	};
	out->pNext = &p->fsr;
}

/* Libraries stay uniform: dynamic rate. */
static void
patch_library(const VkGraphicsPipelineCreateInfo *in,
	VkGraphicsPipelineCreateInfo *out, struct patch *p)
{
	VkGraphicsPipelineLibraryFlagsEXT s = subsets(in);

	if (!(s & RATE_SUBSETS))
		return;
	add_dynamic(in, out, p);
	p->bind_rate = (s & FRAGMENT_SUBSET) && !depth_dynamic(in);
}

/* Scene draws take attachment rate. */
void
drawrate_patch(const VkGraphicsPipelineCreateInfo *in,
	VkGraphicsPipelineCreateInfo *out, struct patch *p)
{
	enum kind k = kind_of(in);

	p->bind_rate = 0;
	if (k == KIND_LINKED)
		return;
	if (k == KIND_LIBRARY) {
		patch_library(in, out, p);
		return;
	}
	if (depth_dynamic(in)) {
		add_dynamic(in, out, p);
		return;
	}
	if (in->renderPass == VK_NULL_HANDLE)
		add_static(in, out, p);
}

static struct recording *
recording_of(VkCommandBuffer cmd)
{
	struct recording *r;
	unsigned i;

	for (i = 0; i < RECENT; i++)
		if (recent[i].cmd == cmd)
			return &recent[i];
	r = &recent[recent_next++ % RECENT];
	*r = (struct recording){ cmd, UNKNOWN, UNKNOWN };
	return r;
}

/* Unknown compare counts as real. */
static void
apply_rate(struct device *dev, VkCommandBuffer cmd, const struct recording *r)
{
	static const VkExtent2D full = { 1, 1 };
	VkFragmentShadingRateCombinerOpKHR ops[2] = {
		VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR,
		r->test == 1 && r->compares ? VK_FRAGMENT_SHADING_RATE_COMBINER_OP_REPLACE_KHR
			: VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR,
	};

	dev->vk.CmdSetFragmentShadingRateKHR(cmd, &full, ops);
}

VKAPI_ATTR VkResult VKAPI_CALL
drawrate_begin(VkCommandBuffer cmd, const VkCommandBufferBeginInfo *info)
{
	struct device *dev = device_of(cmd);
	unsigned i;

	for (i = 0; i < RECENT; i++)
		if (recent[i].cmd == cmd)
			recent[i] = (struct recording){ cmd, UNKNOWN, UNKNOWN };
	return dev->vk.BeginCommandBuffer(cmd, info);
}

/* Static-depth libraries: full rate. */
VKAPI_ATTR void VKAPI_CALL
drawrate_bind(VkCommandBuffer cmd, VkPipelineBindPoint point,
	VkPipeline pipeline)
{
	struct device *dev = device_of(cmd);

	dev->vk.CmdBindPipeline(cmd, point, pipeline);
	if (point != VK_PIPELINE_BIND_POINT_GRAPHICS
			|| !atomic_load_explicit(&dev->bind_rate, memory_order_relaxed))
		return;
	apply_rate(dev, cmd, recording_of(cmd));
}

VKAPI_ATTR void VKAPI_CALL
drawrate_depth_test(VkCommandBuffer cmd, VkBool32 enable)
{
	struct device *dev = device_of(cmd);
	struct recording *r;

	dev->vk.CmdSetDepthTestEnable(cmd, enable);
	if (!dev->vrs)
		return;
	r = recording_of(cmd);
	r->test = enable != VK_FALSE;
	apply_rate(dev, cmd, r);
}

VKAPI_ATTR void VKAPI_CALL
drawrate_depth_compare(VkCommandBuffer cmd, VkCompareOp op)
{
	struct device *dev = device_of(cmd);
	struct recording *r;

	dev->vk.CmdSetDepthCompareOp(cmd, op);
	if (!dev->vrs)
		return;
	r = recording_of(cmd);
	r->compares = (signed char)compares(op);
	apply_rate(dev, cmd, r);
}
