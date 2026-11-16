#ifndef NIXLY_VRS_PASSES_H
#define NIXLY_VRS_PASSES_H

#include "handlemap.h"
#include "layer.h"

#define SUBPASS_MAX 32
#define ATTACH_MAX  32
#define CHAIN_MAX   8
#define DYNAMIC_MAX 96
#define FSR_BIT     VK_PIPELINE_CREATE_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR
#define FSR_BIT2    VK_PIPELINE_CREATE_2_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR
#define UNBOUNDED   UINT32_MAX  /* area outside zoned passes */
#define RATE_BITS   3           /* rate word field width */
#define FACT_RATE   16          /* held rate word shift */
#define RECORDING_HITS 4        /* buffers a thread interleaves */

/* Rate word: size and combiners. */
enum rate_field { FIELD_WIDTH, FIELD_HEIGHT, FIELD_OP0, FIELD_OP1 };

#define RATE_WORD(w, h, op0, op1) ((uint32_t)(w) << FIELD_WIDTH * RATE_BITS \
	| (uint32_t)(h) << FIELD_HEIGHT * RATE_BITS | (uint32_t)(op0) << FIELD_OP0 * RATE_BITS \
	| (uint32_t)(op1) << FIELD_OP1 * RATE_BITS)

/* Pipeline facts; FACT_KNOWN marks presence. */
enum fact {
	FACT_KNOWN          = 1 << 0,
	FACT_UNSAFE         = 1 << 1,   /* must shade per pixel */
	FACT_DEPTH_STATIC   = 1 << 2,
	FACT_DEPTH_TESTED   = 1 << 3,
	FACT_DYNAMIC_RATE   = 1 << 4,   /* rate is dynamic state */
	FACT_TEST_STATIC    = 1 << 5,   /* static half, dynamic depth */
	FACT_TEST_ON        = 1 << 6,
	FACT_COMPARE_STATIC = 1 << 7,
	FACT_COMPARE_REAL   = 1 << 8,
	FACT_VIEWPORT_FIXED = 1 << 9,   /* bind drops set viewport */
	FACT_LAYER_RATE     = 1 << 10,  /* layer made rate dynamic */
	FACT_COUNT_FIXED    = 1 << 11,  /* bind drops set count */
	FACT_OWN_ATTACHMENT = 1 << 12,  /* app flags its own attachment */
	FACT_DISCARD        = 1 << 13,  /* may discard: rate capped */
};

/* Fragment shader object verdict. */
enum object {
	OBJECT_UNSAFE  = 1 << 0,
	OBJECT_OWN     = 1 << 1,        /* app flags its own attachment */
	OBJECT_DISCARD = 1 << 2,        /* may discard: rate capped */
};

/* Fragment shader coarse verdict. */
enum verdict { SPIRV_SAFE, SPIRV_DISCARD, SPIRV_UNSAFE };

/* Combiner the bound pipeline needs. */
enum rate_mode { MODE_DYNAMIC, MODE_KEEP, MODE_DEPTH };

/* Render pass with rate twin. */
struct pass {
	VkRenderPass twin;
	uint32_t depth;             /* subpasses with depth, bitmask */
};

/* Framebuffer plus its twins. */
struct framebuffer {
	VkFramebufferCreateInfo create;
	VkImageView *views;         /* NULL when imageless */
	VkFramebufferAttachmentImageInfo *images;
	VkFramebuffer neutral;
	_Atomic(VkImageView) zoned_view;
	_Atomic(VkFramebuffer) zoned;
};

struct twin {
	VkFramebuffer framebuffer;
	VkImageView view;
};

/* Twin pass and rate view. */
struct twin_spec {
	VkRenderPass pass;
	VkImageView view;
};

/* Native-rate attachment for every pass. */
struct neutral {
	struct gpu_image image;
	VkExtent2D grid;
};

/* Rate-relevant state per command buffer. */
struct recording {
	VkExtent2D area;            /* zoned pass, else unbounded */
	VkViewport viewport;        /* index 0, as last set */
	uint32_t sent;              /* ours in force, 0: unset */
	uint32_t held;              /* app's static rate, 0: none */
	signed char test;
	signed char compares;
	signed char coverage;
	unsigned char mode;         /* bound pipeline's rate_mode */
	unsigned char dynamic;      /* ours may be set now */
	unsigned char layer;        /* bound rate is ours alone */
	unsigned char app;          /* game sets its own rates */
	unsigned char zoned;        /* read a zone map */
	unsigned char multi;        /* several viewports set */
	unsigned char guess;        /* area from swapchain, pass unknown */
	unsigned char own;          /* may meet app's attachment */
	unsigned char fragment;     /* bound fragment object verdict */
	unsigned char discards;     /* bound fragment may discard */
};

struct passes {
	struct handle_map shaders;      /* module, unsafe only */
	struct handle_map objects;      /* shader object, enum object */
	struct handle_map pipelines;    /* pipeline, facts */
	struct handle_map renderpasses; /* render pass, struct pass */
	struct handle_map framebuffers; /* framebuffer, struct framebuffer */
	struct handle_map recordings;   /* command buffer, struct recording */
	struct handle_map pools;        /* command pool, its buffers */
	pthread_mutex_t lock;           /* zoned twins */
	_Atomic int blind;              /* facts lost: stay native */
	struct neutral neutral;
};

/* Secondary begin with twin pass. */
struct inherited {
	VkCommandBufferBeginInfo begin;
	VkCommandBufferInheritanceInfo inheritance;
};

/* Storage behind one patched pipeline. */
struct patch {
	VkPipelineDynamicStateCreateInfo dynamic;
	VkDynamicState states[DYNAMIC_MAX];
	union chain_node chain[CHAIN_MAX];
	uint32_t facts;             /* enum fact bits */
	VkRenderPass twin;          /* rate twin of renderPass */
};

/* Thread's recent buffer lookup. */
struct recording_hit {
	VkCommandBuffer cmd;
	struct recording *recording;
	uint64_t epoch;
};

extern PER_THREAD struct recording_hit recording_hits[RECORDING_HITS];
extern _Atomic uint64_t recording_epoch;  /* bumped per untracked buffer */

struct recording *recording_find(struct device *dev, VkCommandBuffer cmd);

/* Every allocated buffer has one. */
static inline struct recording *
recording_of(struct device *dev, VkCommandBuffer cmd)
{
	uint64_t epoch = atomic_load_explicit(&recording_epoch, memory_order_relaxed);
	const struct recording_hit *h = recording_hits, *end = h + RECORDING_HITS;

	while (h < end && (h->cmd != cmd || h->epoch != epoch))
		h++;
	return h < end ? h->recording : recording_find(dev, cmd);
}

static inline uint32_t
rate_field(uint32_t word, enum rate_field field)
{
	return word >> field * RATE_BITS & ((1u << RATE_BITS) - 1);
}

static inline int
depth_compares(VkCompareOp op)
{
	return op != VK_COMPARE_OP_ALWAYS && op != VK_COMPARE_OP_NEVER;
}

static inline int
is_dynamic(const VkGraphicsPipelineCreateInfo *in, VkDynamicState state)
{
	const VkPipelineDynamicStateCreateInfo *d = in->pDynamicState;
	uint32_t i = 0, n = d ? d->dynamicStateCount : 0;

	while (i < n && d->pDynamicStates[i] != state)
		i++;
	return i < n;
}

int passes_create(struct device *dev, const VkDeviceCreateInfo *requested);
void passes_destroy(struct device *dev);
void passes_check(struct device *dev, VkCommandBuffer cmd);

void neutral_create(struct device *dev, const VkDeviceCreateInfo *requested);
void neutral_destroy(struct device *dev);

enum verdict shader_fragment_verdict(struct device *dev,
	const VkGraphicsPipelineCreateInfo *in);
enum verdict shader_object_verdict(const struct device *dev, const VkShaderCreateInfoEXT *in);
void shader_drop(uint64_t value, void *dev);
VKAPI_ATTR VkResult VKAPI_CALL shader_create(VkDevice device,
	const VkShaderModuleCreateInfo *create, const VkAllocationCallbacks *alloc,
	VkShaderModule *out);
VKAPI_ATTR void VKAPI_CALL shader_destroy(VkDevice device, VkShaderModule module,
	const VkAllocationCallbacks *alloc);

VkImageAspectFlags format_aspects(VkFormat format);
VkRenderPassCreateInfo2 *pass_convert(const VkRenderPassCreateInfo *in);
void pass_convert_free(VkRenderPassCreateInfo2 *pass);

const struct pass *pass_of(struct device *dev, VkRenderPass handle);
int pass_subpass_depth(const struct pass *p, uint32_t subpass);
void pass_drop(uint64_t value, void *dev);
VKAPI_ATTR VkResult VKAPI_CALL renderpass_create(VkDevice device,
	const VkRenderPassCreateInfo *create, const VkAllocationCallbacks *alloc,
	VkRenderPass *out);
VKAPI_ATTR VkResult VKAPI_CALL renderpass_create2(VkDevice device,
	const VkRenderPassCreateInfo2 *create, const VkAllocationCallbacks *alloc,
	VkRenderPass *out);
VKAPI_ATTR void VKAPI_CALL renderpass_destroy(VkDevice device, VkRenderPass handle,
	const VkAllocationCallbacks *alloc);

struct framebuffer *framebuffer_of(struct device *dev, VkFramebuffer handle);
struct twin framebuffer_twin(struct device *dev, struct framebuffer *f,
	struct twin_spec spec);
void framebuffer_drop(uint64_t value, void *dev);
VKAPI_ATTR VkResult VKAPI_CALL framebuffer_create(VkDevice device,
	const VkFramebufferCreateInfo *create, const VkAllocationCallbacks *alloc,
	VkFramebuffer *out);
VKAPI_ATTR void VKAPI_CALL framebuffer_destroy(VkDevice device,
	VkFramebuffer handle, const VkAllocationCallbacks *alloc);

const VkCommandBufferBeginInfo *pass_inherit(struct device *dev,
	const VkCommandBufferBeginInfo *begin, struct inherited *scratch);
VKAPI_ATTR void VKAPI_CALL pass_begin(VkCommandBuffer cmd,
	const VkRenderPassBeginInfo *begin, VkSubpassContents contents);
VKAPI_ATTR void VKAPI_CALL pass_begin2(VkCommandBuffer cmd,
	const VkRenderPassBeginInfo *begin, const VkSubpassBeginInfo *subpass);

int perpixel_samples(const struct device *dev, const VkGraphicsPipelineCreateInfo *in);
int perpixel_raster(const struct device *dev, const VkGraphicsPipelineCreateInfo *in);
int perpixel_objects(const struct device *dev);

uint32_t pipestate_facts(struct device *dev, const VkGraphicsPipelineCreateInfo *in);
enum rate_mode pipestate_mode(uint32_t facts);
void pipestate_patch(const VkGraphicsPipelineCreateInfo *in,
	VkGraphicsPipelineCreateInfo *out, struct patch *p);
int pipestate_kept(const VkGraphicsPipelineCreateInfo *in, const struct patch *p);
VKAPI_ATTR VkResult VKAPI_CALL pipeline_create_graphics(VkDevice device,
	VkPipelineCache cache, uint32_t count,
	const VkGraphicsPipelineCreateInfo *creates,
	const VkAllocationCallbacks *alloc, VkPipeline *out);
VKAPI_ATTR void VKAPI_CALL pipeline_destroy(VkDevice device, VkPipeline pipeline,
	const VkAllocationCallbacks *alloc);

void recording_pool_drop(uint64_t value, void *dev);
VKAPI_ATTR VkResult VKAPI_CALL recording_allocate(VkDevice device,
	const VkCommandBufferAllocateInfo *request, VkCommandBuffer *out);
VKAPI_ATTR void VKAPI_CALL recording_free(VkDevice device, VkCommandPool pool,
	uint32_t count, const VkCommandBuffer *cmds);
VKAPI_ATTR void VKAPI_CALL recording_pool_destroy(VkDevice device, VkCommandPool pool,
	const VkAllocationCallbacks *alloc);

uint32_t drawrule_rate(const struct recording *r);

void drawrate_apply(struct device *dev, VkCommandBuffer cmd, struct recording *r);
void drawrate_area(struct device *dev, VkCommandBuffer cmd, VkExtent2D area);
void drawrate_plain(struct device *dev, VkCommandBuffer cmd);
VKAPI_ATTR VkResult VKAPI_CALL drawrate_begin(VkCommandBuffer cmd,
	const VkCommandBufferBeginInfo *begin);
VKAPI_ATTR void VKAPI_CALL drawrate_bind(VkCommandBuffer cmd,
	VkPipelineBindPoint point, VkPipeline pipeline);
VKAPI_ATTR void VKAPI_CALL drawrate_execute(VkCommandBuffer cmd, uint32_t count,
	const VkCommandBuffer *secondaries);
VKAPI_ATTR void VKAPI_CALL drawrate_depth_test(VkCommandBuffer cmd,
	VkBool32 enable);
VKAPI_ATTR void VKAPI_CALL drawrate_depth_compare(VkCommandBuffer cmd,
	VkCompareOp op);
VKAPI_ATTR void VKAPI_CALL drawrate_coverage(VkCommandBuffer cmd,
	VkBool32 enable);
VKAPI_ATTR void VKAPI_CALL drawrate_app_rate(VkCommandBuffer cmd,
	const VkExtent2D *size, const VkFragmentShadingRateCombinerOpKHR ops[2]);
VKAPI_ATTR void VKAPI_CALL drawrate_viewport(VkCommandBuffer cmd, uint32_t first,
	uint32_t count, const VkViewport *viewports);
VKAPI_ATTR void VKAPI_CALL drawrate_viewport_count(VkCommandBuffer cmd,
	uint32_t count, const VkViewport *viewports);

VKAPI_ATTR VkResult VKAPI_CALL shaderobject_create(VkDevice device, uint32_t count,
	const VkShaderCreateInfoEXT *creates, const VkAllocationCallbacks *alloc,
	VkShaderEXT *out);
VKAPI_ATTR void VKAPI_CALL shaderobject_destroy(VkDevice device, VkShaderEXT shader,
	const VkAllocationCallbacks *alloc);
VKAPI_ATTR void VKAPI_CALL shaderobject_bind(VkCommandBuffer cmd, uint32_t count,
	const VkShaderStageFlagBits *stages, const VkShaderEXT *shaders);

int scene_shaped(const struct device *dev, VkExtent2D extent);
int scene_inherited(const VkCommandBufferInheritanceInfo *in);
VKAPI_ATTR void VKAPI_CALL rendering_begin(VkCommandBuffer cmd,
	const VkRenderingInfo *rendering);

#endif
