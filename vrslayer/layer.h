#ifndef NIXLY_VRS_LAYER_H
#define NIXLY_VRS_LAYER_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <time.h>
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#include "control.h"
#include "link.h"
#include "pattern.h"

#define EXPORT __attribute__((visibility("default")))

#define RATE_SIZES    8
#define UPLOAD_SLOTS  4
#define TIMING_SLOTS  64
#define QUEUES_MAX    32
#define FAMILIES_MAX  16
#define ACQUIRED_MAX  8
#define QUERY_COUNT   (2 * TIMING_SLOTS)
#define CMD_COUNT     (2 * TIMING_SLOTS + UPLOAD_SLOTS)
#define CHAIN_MAX     8
#define NODE_BYTES    128
#define DYNAMIC_MAX   96

#define INSTANCE_FUNCS(X) \
	X(DestroyInstance) \
	X(EnumerateDeviceExtensionProperties) \
	X(GetPhysicalDeviceFeatures2) \
	X(GetPhysicalDeviceProperties2) \
	X(GetPhysicalDeviceQueueFamilyProperties) \
	X(GetPhysicalDeviceMemoryProperties) \
	X(GetPhysicalDeviceFragmentShadingRatesKHR)

#define DEVICE_FUNCS(X) \
	X(DestroyDevice) X(DeviceWaitIdle) X(GetDeviceQueue) X(GetDeviceQueue2) \
	X(CreateSwapchainKHR) X(QueueSubmit) X(QueueSubmit2) X(QueuePresentKHR) \
	X(CmdBeginRendering) X(CreateGraphicsPipelines) \
	X(CreateImage) X(DestroyImage) X(CreateImageView) X(DestroyImageView) \
	X(GetImageMemoryRequirements) X(BindImageMemory) \
	X(CreateBuffer) X(DestroyBuffer) X(GetBufferMemoryRequirements) \
	X(BindBufferMemory) X(AllocateMemory) X(FreeMemory) X(MapMemory) \
	X(CreateCommandPool) X(DestroyCommandPool) X(AllocateCommandBuffers) \
	X(BeginCommandBuffer) X(EndCommandBuffer) X(CmdPipelineBarrier) \
	X(CmdCopyBufferToImage) X(CmdResetQueryPool) X(CmdWriteTimestamp) \
	X(CmdFillBuffer) X(CreateSemaphore) X(DestroySemaphore) \
	X(CmdBindPipeline) X(CmdSetDepthTestEnable) X(CmdSetDepthCompareOp) \
	X(CmdSetFragmentShadingRateKHR) \
	X(AcquireNextImageKHR) X(AcquireNextImage2KHR) \
	X(CreateQueryPool) X(DestroyQueryPool) X(GetQueryPoolResults)

#define PFN_FIELD(name) PFN_vk##name name;

struct instance {
	VkInstance handle;
	uint32_t api_version;
	PFN_vkGetInstanceProcAddr gipa;
	INSTANCE_FUNCS(PFN_FIELD)
};

enum rate_state { RATE_EMPTY, RATE_FRESH, RATE_UPLOADING, RATE_READY, RATE_FAILED };

/* Copied pNext node. */
union chain_node {
	VkBaseOutStructure base;
	uint64_t align;
	unsigned char bytes[NODE_BYTES];
};

/* Rate image per render size. */
struct rate_image {
	_Atomic int state;
	VkExtent2D size;
	VkExtent2D grid;
	VkImage image;
	VkImageView view;
	VkDeviceMemory memory;
	VkBuffer staging;
	VkDeviceMemory staging_memory;
	uint8_t *mapped;
	VkDeviceSize slot_bytes;
	uint32_t upload_mark;       /* timing mark of first upload */
	int initialized;
	uint64_t painted;           /* params of the last paint */
};

/* Present queue busy-time ring. */
struct timing {
	VkCommandBuffer begin[TIMING_SLOTS];
	VkCommandBuffer end[TIMING_SLOTS];
	VkSemaphore hop[TIMING_SLOTS];     /* re-signals acquire waits */
	VkBuffer scratch;
	VkDeviceMemory scratch_memory;
	uint64_t frame[TIMING_SLOTS];
	int level[TIMING_SLOTS];
	uint32_t head, tail;
	uint64_t sum_frame;
	float sum_ns;
	int sum_level;
	float period;
	uint64_t mask;
	uint64_t last_end;
};

struct uploads {
	VkCommandBuffer cmd[UPLOAD_SLOTS];
	int used[UPLOAD_SLOTS];
	uint32_t mark[UPLOAD_SLOTS];
	uint32_t seq;
};

/* Storage behind one patched pipeline. */
struct patch {
	VkPipelineFragmentShadingRateStateCreateInfoKHR fsr;
	VkPipelineDynamicStateCreateInfo dynamic;
	VkDynamicState states[DYNAMIC_MAX];
	union chain_node chain[CHAIN_MAX];
	int bind_rate;              /* rate set at bind */
};

/* Work injected into one submit. */
struct extras {
	VkCommandBuffer upload;
	VkCommandBuffer begin;
	VkCommandBuffer end;
	VkSemaphore hop;
};

struct queue_info {
	VkQueue queue;
	uint32_t family;
};

struct device {
	VkDevice handle;
	VkPhysicalDevice phys;
	struct instance *inst;
	PFN_vkGetDeviceProcAddr gdpa;
	PFN_vkSetDeviceLoaderData set_loader_data;
	struct {
		DEVICE_FUNCS(PFN_FIELD)
	} vk;
	int vrs;
	_Atomic int unsafe;         /* unpatched pipeline exists */
	_Atomic int bind_rate;      /* static-depth library seen */
	VkExtent2D texel;
	uint8_t codes[PATTERN_RINGS];
	VkPhysicalDeviceMemoryProperties memory;
	VkQueueFamilyProperties families[FAMILIES_MAX];
	uint32_t nfamilies;
	float timestamp_period;
	struct queue_info queues[QUEUES_MAX];
	_Atomic uint32_t nqueues;
	pthread_mutex_t lock;
	_Atomic(VkQueue) present;   /* timing and upload queue */
	_Atomic int present_failed;
	_Atomic uint64_t swap_extent;
	_Atomic int level;
	VkCommandPool cmd_pool;
	VkQueryPool queries;
	struct timing timing;
	struct uploads uploads;
	struct rate_image rates[RATE_SIZES];
	struct control control;
	uint64_t frames;
	VkSemaphore acquired[ACQUIRED_MAX];
	uint32_t nacquired;
};

/* Device patch staged for vkCreateDevice. */
struct device_setup {
	struct instance *inst;
	VkPhysicalDevice phys;
	const VkDeviceCreateInfo *app;
	VkDeviceCreateInfo info;
	VkPhysicalDeviceFragmentShadingRateFeaturesKHR features;
	const char **extensions;
	VkExtent2D texel;
	uint8_t codes[PATTERN_RINGS];
};

static inline uint64_t
now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static inline const VkBaseInStructure *
chain_find(const void *chain, VkStructureType type)
{
	const VkBaseInStructure *s = chain;

	while (s && s->sType != type)
		s = s->pNext;
	return s;
}

int registry_add(const void *handle, void *object);
void registry_remove(const void *handle);
void *registry_find(const void *handle);

struct instance *instance_of(const void *handle);
VKAPI_ATTR VkResult VKAPI_CALL instance_create(const VkInstanceCreateInfo *info,
	const VkAllocationCallbacks *alloc, VkInstance *out);
VKAPI_ATTR void VKAPI_CALL instance_destroy(VkInstance instance,
	const VkAllocationCallbacks *alloc);

int features_prepare(struct device_setup *s);
void features_release(struct device_setup *s);

struct device *device_of(const void *handle);
uint32_t device_family_of(struct device *dev, VkQueue queue);
VkDeviceMemory device_allocate(struct device *dev, VkMemoryRequirements req,
	VkMemoryPropertyFlags flags);
uint32_t device_memory_type(const struct device *dev, uint32_t bits,
	VkMemoryPropertyFlags flags);
VKAPI_ATTR VkResult VKAPI_CALL device_create(VkPhysicalDevice phys,
	const VkDeviceCreateInfo *info, const VkAllocationCallbacks *alloc,
	VkDevice *out);
VKAPI_ATTR void VKAPI_CALL device_destroy(VkDevice device,
	const VkAllocationCallbacks *alloc);
VKAPI_ATTR void VKAPI_CALL device_get_queue(VkDevice device, uint32_t family,
	uint32_t index, VkQueue *out);
VKAPI_ATTR void VKAPI_CALL device_get_queue2(VkDevice device,
	const VkDeviceQueueInfo2 *info, VkQueue *out);

VKAPI_ATTR VkResult VKAPI_CALL pipeline_create_graphics(VkDevice device,
	VkPipelineCache cache, uint32_t count,
	const VkGraphicsPipelineCreateInfo *infos,
	const VkAllocationCallbacks *alloc, VkPipeline *out);

void drawrate_patch(const VkGraphicsPipelineCreateInfo *in,
	VkGraphicsPipelineCreateInfo *out, struct patch *p);
VKAPI_ATTR VkResult VKAPI_CALL drawrate_begin(VkCommandBuffer cmd,
	const VkCommandBufferBeginInfo *info);
VKAPI_ATTR void VKAPI_CALL drawrate_bind(VkCommandBuffer cmd,
	VkPipelineBindPoint point, VkPipeline pipeline);
VKAPI_ATTR void VKAPI_CALL drawrate_depth_test(VkCommandBuffer cmd,
	VkBool32 enable);
VKAPI_ATTR void VKAPI_CALL drawrate_depth_compare(VkCommandBuffer cmd,
	VkCompareOp op);

VkImageView rate_view(struct device *dev, VkExtent2D size);
void rate_settle(struct device *dev);
void rate_destroy(struct device *dev);

VKAPI_ATTR void VKAPI_CALL rendering_begin(VkCommandBuffer cmd,
	const VkRenderingInfo *info);

VkCommandBuffer upload_changes(struct device *dev);

int timing_init(struct device *dev);
void timing_destroy(struct device *dev);
void timing_claim(struct device *dev, struct extras *x);
void timing_abandon(struct device *dev, const struct extras *x);
void timing_harvest(struct device *dev, uint64_t now);
uint32_t timing_mark(const struct device *dev);
int timing_passed(const struct device *dev, uint32_t mark);

VKAPI_ATTR VkResult VKAPI_CALL frame_swapchain(VkDevice device,
	const VkSwapchainCreateInfoKHR *info, const VkAllocationCallbacks *alloc,
	VkSwapchainKHR *out);
VKAPI_ATTR VkResult VKAPI_CALL frame_present(VkQueue queue,
	const VkPresentInfoKHR *info);
VKAPI_ATTR VkResult VKAPI_CALL frame_acquire(VkDevice device,
	VkSwapchainKHR swapchain, uint64_t timeout, VkSemaphore semaphore,
	VkFence fence, uint32_t *index);
VKAPI_ATTR VkResult VKAPI_CALL frame_acquire2(VkDevice device,
	const VkAcquireNextImageInfoKHR *info, uint32_t *index);
int frame_acquired(struct device *dev, VkSemaphore semaphore);
void frame_destroy(struct device *dev);

VKAPI_ATTR VkResult VKAPI_CALL submit_queue(VkQueue queue, uint32_t count,
	const VkSubmitInfo *submits, VkFence fence);
VKAPI_ATTR VkResult VKAPI_CALL submit_queue2(VkQueue queue, uint32_t count,
	const VkSubmitInfo2 *submits, VkFence fence);

void link_connect(void);
struct vrs_link *link_page(void);

#endif
