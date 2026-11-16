#include "passes.h"

#define UNKNOWN   (-1)
#define UNSENT    0u
#define KEEP      VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR

static const struct recording fresh = {
	.area = { UNBOUNDED, UNBOUNDED },
	.sent = UNSENT,
	.test = UNKNOWN, .compares = UNKNOWN, .coverage = UNKNOWN, .mode = MODE_DYNAMIC,
};

static const VkExtent2D full_rate = { 1, 1 };

/* Rate word to driver. */
static void
emit(struct device *dev, VkCommandBuffer cmd, uint32_t word)
{
	VkExtent2D size = { rate_field(word, FIELD_WIDTH), rate_field(word, FIELD_HEIGHT) };
	VkFragmentShadingRateCombinerOpKHR ops[2] = {
		(VkFragmentShadingRateCombinerOpKHR)rate_field(word, FIELD_OP0),
		(VkFragmentShadingRateCombinerOpKHR)rate_field(word, FIELD_OP1),
	};

	dev->vk.CmdSetFragmentShadingRateKHR(cmd, &size, ops);
}

/* Changes only; static pipelines forbid. */
void
drawrate_apply(struct device *dev, VkCommandBuffer cmd, struct recording *r)
{
	uint32_t word;

	if (!r->dynamic || (r->app && !r->layer))
		return;
	word = drawrule_rate(r);
	if (r->sent == word)
		return;
	r->sent = word;
	emit(dev, cmd, word);
}

/* Static, blind binds void rate. */
static void
learn_bound(struct device *dev, struct recording *r, VkPipeline pipeline)
{
	uint32_t facts;

	r->fragment = 0;
	if (atomic_load_explicit(&dev->passes->blind, memory_order_relaxed)) {
		r->mode = MODE_KEEP;
		r->dynamic = 1;
		r->layer = 0;
		r->held = 0;
		r->sent = UNSENT;
		return;
	}
	facts = (uint32_t)handlemap_get(&dev->passes->pipelines, (uint64_t)pipeline);
	r->mode = (unsigned char)pipestate_mode(facts);
	r->dynamic = (facts & FACT_DYNAMIC_RATE) != 0;
	r->layer = (facts & FACT_LAYER_RATE) != 0;
	r->held = facts >> FACT_RATE;
	r->own = (facts & FACT_OWN_ATTACHMENT) != 0;
	r->discards = (facts & FACT_DISCARD) != 0;
	if (!r->dynamic)
		r->sent = UNSENT;
	if (facts & FACT_TEST_STATIC)
		r->test = (facts & FACT_TEST_ON) != 0;
	if (facts & FACT_COMPARE_STATIC)
		r->compares = (facts & FACT_COMPARE_REAL) != 0;
	if (facts & FACT_VIEWPORT_FIXED)
		r->viewport = fresh.viewport;
	if (facts & FACT_COUNT_FIXED)
		r->multi = 0;
}

/* Secondaries: framebuffer, else swapchain. */
static void
inherit_area(struct device *dev, struct recording *r,
	const VkCommandBufferBeginInfo *begin)
{
	const VkCommandBufferInheritanceInfo *in = begin->pInheritanceInfo;
	struct framebuffer *f;
	VkExtent2D swap;
	int ours;

	if (!(begin->flags & VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT) || !in)
		return;
	f = framebuffer_of(dev, in->framebuffer);
	if (f) {
		r->area = (VkExtent2D){ f->create.width, f->create.height };
		return;
	}
	swap = extent_of(atomic_load(&dev->swap_extent));
	ours = pass_of(dev, in->renderPass) != NULL;
	if (!swap.width || !(ours || scene_inherited(in)))
		return;
	r->area = swap;
	r->guess = !ours;
}

VKAPI_ATTR VkResult VKAPI_CALL
drawrate_begin(VkCommandBuffer cmd, const VkCommandBufferBeginInfo *begin)
{
	struct device *dev = device_of(cmd);
	struct recording *r = recording_of(dev, cmd);
	struct inherited scratch;

	*r = fresh;
	inherit_area(dev, r, begin);
	return dev->vk.BeginCommandBuffer(cmd, pass_inherit(dev, begin, &scratch));
}

/* Dynamic-rate pipelines: set now. */
VKAPI_ATTR void VKAPI_CALL
drawrate_bind(VkCommandBuffer cmd, VkPipelineBindPoint point,
	VkPipeline pipeline)
{
	struct device *dev = device_of(cmd);
	struct recording *r;

	dev->vk.CmdBindPipeline(cmd, point, pipeline);
	if (point != VK_PIPELINE_BIND_POINT_GRAPHICS
			|| !atomic_load_explicit(&dev->bind_rate, memory_order_relaxed))
		return;
	r = recording_of(dev, cmd);
	learn_bound(dev, r, pipeline);
	drawrate_apply(dev, cmd, r);
}

/* Leaves primary state undefined. */
VKAPI_ATTR void VKAPI_CALL
drawrate_execute(VkCommandBuffer cmd, uint32_t count,
	const VkCommandBuffer *secondaries)
{
	struct device *dev = device_of(cmd);

	dev->vk.CmdExecuteCommands(cmd, count, secondaries);
	recording_of(dev, cmd)->sent = UNSENT;
}

VKAPI_ATTR void VKAPI_CALL
drawrate_depth_test(VkCommandBuffer cmd, VkBool32 enable)
{
	struct device *dev = device_of(cmd);
	struct recording *r = recording_of(dev, cmd);

	dev->vk.CmdSetDepthTestEnable(cmd, enable);
	r->test = enable != VK_FALSE;
	drawrate_apply(dev, cmd, r);
}

VKAPI_ATTR void VKAPI_CALL
drawrate_depth_compare(VkCommandBuffer cmd, VkCompareOp op)
{
	struct device *dev = device_of(cmd);
	struct recording *r = recording_of(dev, cmd);

	dev->vk.CmdSetDepthCompareOp(cmd, op);
	r->compares = depth_compares(op);
	drawrate_apply(dev, cmd, r);
}

VKAPI_ATTR void VKAPI_CALL
drawrate_coverage(VkCommandBuffer cmd, VkBool32 enable)
{
	struct device *dev = device_of(cmd);
	struct recording *r = recording_of(dev, cmd);

	dev->vk.CmdSetAlphaToCoverageEnableEXT(cmd, enable);
	r->coverage = enable != VK_FALSE;
	drawrate_apply(dev, cmd, r);
}

/* Game's rate holds until rebind. */
VKAPI_ATTR void VKAPI_CALL
drawrate_app_rate(VkCommandBuffer cmd, const VkExtent2D *size,
	const VkFragmentShadingRateCombinerOpKHR ops[2])
{
	struct device *dev = device_of(cmd);
	struct recording *r = recording_of(dev, cmd);
	int neutral = size->width == full_rate.width && size->height == full_rate.height
		&& ops[0] == KEEP && ops[1] == KEEP;

	if (!r->app && neutral) {
		drawrate_apply(dev, cmd, r);
		return;
	}
	r->app = 1;
	r->dynamic = 0;
	r->sent = UNSENT;
	dev->vk.CmdSetFragmentShadingRateKHR(cmd, size, ops);
}

/* Zoned pass: map spans area. */
void
drawrate_area(struct device *dev, VkCommandBuffer cmd, VkExtent2D area)
{
	struct recording *r = recording_of(dev, cmd);

	r->area = area;
	r->zoned = 1;
	drawrate_apply(dev, cmd, r);
}

/* Unzoned pass: native or app's. */
void
drawrate_plain(struct device *dev, VkCommandBuffer cmd)
{
	struct recording *r = recording_of(dev, cmd);

	r->area = fresh.area;
	drawrate_apply(dev, cmd, r);
}

VKAPI_ATTR void VKAPI_CALL
drawrate_viewport(VkCommandBuffer cmd, uint32_t first, uint32_t count,
	const VkViewport *viewports)
{
	struct device *dev = device_of(cmd);
	struct recording *r;

	dev->vk.CmdSetViewport(cmd, first, count, viewports);
	if (first)
		return;
	r = recording_of(dev, cmd);
	r->viewport = viewports[0];
	drawrate_apply(dev, cmd, r);
}

/* Tracks dynamic viewport count. */
VKAPI_ATTR void VKAPI_CALL
drawrate_viewport_count(VkCommandBuffer cmd, uint32_t count,
	const VkViewport *viewports)
{
	struct device *dev = device_of(cmd);
	struct recording *r = recording_of(dev, cmd);

	dev->vk.CmdSetViewportWithCount(cmd, count, viewports);
	r->viewport = viewports[0];
	r->multi = count > 1;
	drawrate_apply(dev, cmd, r);
}
