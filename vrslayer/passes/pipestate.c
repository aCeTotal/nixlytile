#include "passes.h"

#include <string.h>

#define FSR_DYNAMIC     VK_DYNAMIC_STATE_FRAGMENT_SHADING_RATE_KHR
#define FSR_STATE       VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_STATE_CREATE_INFO_KHR
#define FSR_ENUM_NV     VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_ENUM_STATE_CREATE_INFO_NV
#define LIBRARY_BIT2    VK_PIPELINE_CREATE_2_LIBRARY_BIT_KHR
#define FRAGMENT_SUBSET VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT
#define ALL_SUBSETS     (VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT \
	| VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT \
	| FRAGMENT_SUBSET | VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT)
#define OUTPUT_SUBSET   VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT
#define SAMPLE_SUBSETS  (FRAGMENT_SUBSET | OUTPUT_SUBSET)
#define RATE_SUBSETS    (VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT \
	| FRAGMENT_SUBSET)
#define RASTER_SUBSET   VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT

enum kind { KIND_COMPLETE, KIND_LIBRARY, KIND_LINKED };

/* Flags2 when chained, else flags. */
static VkPipelineCreateFlags2
create_flags(const VkGraphicsPipelineCreateInfo *in)
{
	const VkPipelineCreateFlags2CreateInfo *f2 = (const void *)
		chain_find(in->pNext, VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO);

	return f2 ? f2->flags : in->flags;
}

static enum kind
kind_of(const VkGraphicsPipelineCreateInfo *in)
{
	const VkPipelineLibraryCreateInfoKHR *libs = (const void *)
		chain_find(in->pNext, VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR);

	if (create_flags(in) & LIBRARY_BIT2)
		return KIND_LIBRARY;
	return libs && libs->libraryCount ? KIND_LINKED : KIND_COMPLETE;
}

/* Subsets this create holds. */
static VkGraphicsPipelineLibraryFlagsEXT
own_subsets(const VkGraphicsPipelineCreateInfo *in)
{
	const VkGraphicsPipelineLibraryCreateInfoEXT *l = (const void *)
		chain_find(in->pNext, VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT);

	if (l)
		return l->flags;
	return kind_of(in) == KIND_COMPLETE ? ALL_SUBSETS : 0;
}

static int
depth_dynamic(const VkGraphicsPipelineCreateInfo *in)
{
	return is_dynamic(in, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE)
		|| is_dynamic(in, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP);
}

static int
viewport_dynamic(const VkGraphicsPipelineCreateInfo *in)
{
	return is_dynamic(in, VK_DYNAMIC_STATE_VIEWPORT)
		|| is_dynamic(in, VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT);
}

/* Static rate reading an attachment. */
static uint32_t
static_rate(const VkGraphicsPipelineCreateInfo *in)
{
	const VkPipelineFragmentShadingRateStateCreateInfoKHR *s = (const void *)
		chain_find(in->pNext, FSR_STATE);

	if (!s || s->combinerOps[1] == VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR
			|| is_dynamic(in, FSR_DYNAMIC) || chain_find(in->pNext, FSR_ENUM_NV))
		return 0;
	return RATE_WORD(s->fragmentSize.width, s->fragmentSize.height,
		s->combinerOps[0], s->combinerOps[1]);
}

/* App drives VRS, unless held. */
static int
app_rate(const VkGraphicsPipelineCreateInfo *in)
{
	return !static_rate(in) && (is_dynamic(in, FSR_DYNAMIC)
		|| chain_find(in->pNext, FSR_STATE) || chain_find(in->pNext, FSR_ENUM_NV));
}

/* Held rate, own attachment flag. */
static uint32_t
app_facts(const VkGraphicsPipelineCreateInfo *in, VkGraphicsPipelineLibraryFlagsEXT own)
{
	uint32_t facts = own & RATE_SUBSETS ? static_rate(in) << FACT_RATE : 0;

	if (create_flags(in) & FSR_BIT2)
		facts |= FACT_OWN_ATTACHMENT;
	return facts;
}

static uint32_t
raster_facts(const struct device *dev, const VkGraphicsPipelineCreateInfo *in)
{
	return (perpixel_raster(dev, in) ? FACT_UNSAFE : 0)
		| (viewport_dynamic(in) ? 0 : FACT_VIEWPORT_FIXED)
		| (is_dynamic(in, VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT) ? 0 : FACT_COUNT_FIXED);
}

static int
depth_attachment(struct device *dev, const VkGraphicsPipelineCreateInfo *in)
{
	const VkPipelineRenderingCreateInfo *r = (const void *)
		chain_find(in->pNext, VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO);

	if (in->renderPass)
		return pass_subpass_depth(pass_of(dev, in->renderPass), in->subpass);
	return r && r->depthAttachmentFormat != VK_FORMAT_UNDEFINED;
}

/* Depth-tested draws: scene, not UI. */
static int
depth_tested(struct device *dev, const VkGraphicsPipelineCreateInfo *in)
{
	const VkPipelineDepthStencilStateCreateInfo *ds = in->pDepthStencilState;

	return ds && ds->depthTestEnable && depth_compares(ds->depthCompareOp)
		&& depth_attachment(dev, in);
}

/* Static half of dynamic depth. */
static uint32_t
static_halves(struct device *dev, const VkGraphicsPipelineCreateInfo *in)
{
	const VkPipelineDepthStencilStateCreateInfo *ds = in->pDepthStencilState;
	uint32_t facts = 0;

	if (!is_dynamic(in, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE))
		facts |= FACT_TEST_STATIC | (ds && ds->depthTestEnable
			&& depth_attachment(dev, in) ? FACT_TEST_ON : 0);
	if (!is_dynamic(in, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP))
		facts |= FACT_COMPARE_STATIC
			| (ds && depth_compares(ds->depthCompareOp) ? FACT_COMPARE_REAL : 0);
	return facts;
}

static uint32_t
library_facts(struct device *dev, const VkGraphicsPipelineCreateInfo *in)
{
	const VkPipelineLibraryCreateInfoKHR *libs = (const void *)
		chain_find(in->pNext, VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR);
	uint32_t i, facts = 0;

	for (i = 0; libs && i < libs->libraryCount; i++)
		facts |= (uint32_t)handlemap_get(&dev->passes->pipelines,
			(uint64_t)libs->pLibraries[i]);
	return facts;
}

/* Discarding shaders stay coarse, capped. */
static const uint32_t fact_of[] = {
	[SPIRV_SAFE] = 0, [SPIRV_DISCARD] = FACT_DISCARD, [SPIRV_UNSAFE] = FACT_UNSAFE,
};

/* Fragment-state facts plus libraries'. */
uint32_t
pipestate_facts(struct device *dev, const VkGraphicsPipelineCreateInfo *in)
{
	VkGraphicsPipelineLibraryFlagsEXT own = own_subsets(in);
	uint32_t facts = FACT_KNOWN | library_facts(dev, in) | app_facts(in, own);

	if ((own & SAMPLE_SUBSETS) && perpixel_samples(dev, in))
		facts |= FACT_UNSAFE;
	if (own & RASTER_SUBSET)
		facts |= raster_facts(dev, in);
	if (!(own & FRAGMENT_SUBSET))
		return facts;
	facts |= fact_of[shader_fragment_verdict(dev, in)];
	if (depth_dynamic(in))
		return facts | static_halves(dev, in);
	return facts | FACT_DEPTH_STATIC | (depth_tested(dev, in) ? FACT_DEPTH_TESTED : 0);
}

enum rate_mode
pipestate_mode(uint32_t facts)
{
	if (!facts)
		return MODE_DYNAMIC;
	if (facts & FACT_UNSAFE)
		return MODE_KEEP;
	if (!(facts & FACT_DEPTH_STATIC))
		return MODE_DYNAMIC;
	return facts & FACT_DEPTH_TESTED ? MODE_DEPTH : MODE_KEEP;
}

static void
add_dynamic(const VkGraphicsPipelineCreateInfo *in,
	VkGraphicsPipelineCreateInfo *out, struct patch *p)
{
	const VkPipelineDynamicStateCreateInfo *d = in->pDynamicState;
	uint32_t n = d ? d->dynamicStateCount : 0;

	if (n >= DYNAMIC_MAX)
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
	p->facts |= FACT_DYNAMIC_RATE | FACT_LAYER_RATE;
}

/* Libraries stay uniform; bind picks. */
static void
patch_library(const VkGraphicsPipelineCreateInfo *in,
	VkGraphicsPipelineCreateInfo *out, struct patch *p)
{
	if (own_subsets(in) & RATE_SUBSETS)
		add_dynamic(in, out, p);
}

/* Scene and held rates dynamic. */
void
pipestate_patch(const VkGraphicsPipelineCreateInfo *in,
	VkGraphicsPipelineCreateInfo *out, struct patch *p)
{
	enum kind k = kind_of(in);

	if (is_dynamic(in, FSR_DYNAMIC))
		p->facts |= FACT_DYNAMIC_RATE;
	if (p->twin)
		out->renderPass = p->twin;
	if (k == KIND_LINKED || app_rate(in))
		return;
	if (k == KIND_LIBRARY) {
		patch_library(in, out, p);
		return;
	}
	if (pipestate_mode(p->facts) != MODE_KEEP || p->facts >> FACT_RATE)
		add_dynamic(in, out, p);
}

/* Libraries, and dynamic-rate pipelines. */
int
pipestate_kept(const VkGraphicsPipelineCreateInfo *in, const struct patch *p)
{
	return kind_of(in) == KIND_LIBRARY || (p->facts & FACT_DYNAMIC_RATE);
}
