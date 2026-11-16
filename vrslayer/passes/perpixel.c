#include "passes.h"

#define ATOC_DYNAMIC      VK_DYNAMIC_STATE_ALPHA_TO_COVERAGE_ENABLE_EXT
#define SAMPLES_DYNAMIC   VK_DYNAMIC_STATE_RASTERIZATION_SAMPLES_EXT
#define LOCATIONS_INFO    VK_STRUCTURE_TYPE_PIPELINE_SAMPLE_LOCATIONS_STATE_CREATE_INFO_EXT
#define CONSERVATIVE_INFO \
	VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_CONSERVATIVE_STATE_CREATE_INFO_EXT
#define MASK_BITS         32    /* one pSampleMask word */

/* Coarse coverage dither is noise. */
static int
per_sample(const VkGraphicsPipelineCreateInfo *in)
{
	const VkPipelineMultisampleStateCreateInfo *ms = in->pMultisampleState;

	return ms && (ms->sampleShadingEnable
		|| (ms->alphaToCoverageEnable && !is_dynamic(in, ATOC_DYNAMIC)));
}

static int
limited(const struct device *dev, enum native_state state)
{
	return (dev->limits.native & state) != 0;
}

/* Samples beyond coarse limit. */
static int
oversampled(const struct device *dev, const VkGraphicsPipelineCreateInfo *in)
{
	const VkPipelineMultisampleStateCreateInfo *ms = in->pMultisampleState;

	return ms && !is_dynamic(in, SAMPLES_DYNAMIC)
		&& ms->rasterizationSamples > dev->limits.samples;
}

/* Custom sample locations, or unknown. */
static int
located(const VkGraphicsPipelineCreateInfo *in)
{
	const VkPipelineMultisampleStateCreateInfo *ms = in->pMultisampleState;
	const VkPipelineSampleLocationsStateCreateInfoEXT *at = ms ? (const void *)
		chain_find(ms->pNext, LOCATIONS_INFO) : NULL;

	return is_dynamic(in, VK_DYNAMIC_STATE_SAMPLE_LOCATIONS_ENABLE_EXT)
		|| (at && at->sampleLocationsEnable);
}

/* Mask drops samples, or unknown. */
static int
masked(const VkGraphicsPipelineCreateInfo *in)
{
	const VkPipelineMultisampleStateCreateInfo *ms = in->pMultisampleState;
	uint32_t n, used;

	if (is_dynamic(in, VK_DYNAMIC_STATE_SAMPLE_MASK_EXT))
		return 1;
	if (!ms || !ms->pSampleMask)
		return 0;
	n = is_dynamic(in, SAMPLES_DYNAMIC) ? MASK_BITS : ms->rasterizationSamples;
	used = n >= MASK_BITS ? UINT32_MAX : (1u << n) - 1;
	return (ms->pSampleMask[0] & used) != used;
}

/* Conservative raster, or unknown. */
static int
conservative(const VkGraphicsPipelineCreateInfo *in)
{
	const VkPipelineRasterizationStateCreateInfo *rs = in->pRasterizationState;
	const VkPipelineRasterizationConservativeStateCreateInfoEXT *c = rs ? (const void *)
		chain_find(rs->pNext, CONSERVATIVE_INFO) : NULL;

	return is_dynamic(in, VK_DYNAMIC_STATE_CONSERVATIVE_RASTERIZATION_MODE_EXT)
		|| (c && c->conservativeRasterizationMode
			!= VK_CONSERVATIVE_RASTERIZATION_MODE_DISABLED_EXT);
}

/* NVK Turing collapses viewports. */
static int
viewports_many(const VkGraphicsPipelineCreateInfo *in)
{
	return in->pViewportState && in->pViewportState->viewportCount > 1;
}

int
perpixel_samples(const struct device *dev, const VkGraphicsPipelineCreateInfo *in)
{
	return per_sample(in) || oversampled(dev, in)
		|| (limited(dev, NATIVE_LOCATIONS) && located(in))
		|| (limited(dev, NATIVE_SAMPLE_MASK) && masked(in));
}

int
perpixel_raster(const struct device *dev, const VkGraphicsPipelineCreateInfo *in)
{
	return viewports_many(in) || (limited(dev, NATIVE_CONSERVATIVE) && conservative(in));
}

/* Objects: masks, locations, raster dynamic. */
int
perpixel_objects(const struct device *dev)
{
	return limited(dev, NATIVE_SAMPLE_MASK | NATIVE_LOCATIONS | NATIVE_CONSERVATIVE);
}
