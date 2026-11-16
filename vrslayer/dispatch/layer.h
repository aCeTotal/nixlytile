#ifndef NIXLY_VRS_LAYER_H
#define NIXLY_VRS_LAYER_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#include "fovea.h"
#include "gpu.h"
#include "link.h"

#define EXPORT __attribute__((visibility("default")))
#define LENGTH(a) (sizeof(a) / sizeof((a)[0]))
/* Static TLS: no __tls_get_addr. */
#define PER_THREAD _Thread_local __attribute__((tls_model("initial-exec")))

#define QUEUES_MAX    32
#define FAMILIES_MAX  16
#define NODE_BYTES    128
#define EXTENT_SHIFT  32            /* width in high half */

#define INSTANCE_FUNCS(X) \
	X(DestroyInstance) \
	X(EnumerateDeviceExtensionProperties) \
	X(GetPhysicalDeviceFeatures2) \
	X(GetPhysicalDeviceProperties2) \
	X(GetPhysicalDeviceQueueFamilyProperties) \
	X(GetPhysicalDeviceMemoryProperties) \
	X(GetPhysicalDeviceFormatProperties) \
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
	X(CmdCopyBufferToImage) X(FreeCommandBuffers) \
	X(CreateFence) X(DestroyFence) X(ResetFences) X(GetFenceStatus) X(WaitForFences) \
	X(CreateShaderModule) X(DestroyShaderModule) X(DestroyPipeline) \
	X(CmdBindPipeline) X(CmdSetDepthTestEnable) X(CmdSetDepthCompareOp) \
	X(CmdSetFragmentShadingRateKHR) X(CmdSetAlphaToCoverageEnableEXT) \
	X(CmdSetViewport) X(CmdSetViewportWithCount) X(CmdExecuteCommands) \
	X(CreateShadersEXT) X(DestroyShaderEXT) X(CmdBindShadersEXT)

/* Classic render passes. */
#define PASS_FUNCS(X) \
	X(CreateRenderPass) X(CreateRenderPass2) X(DestroyRenderPass) \
	X(CreateFramebuffer) X(DestroyFramebuffer) \
	X(CmdBeginRenderPass) X(CmdBeginRenderPass2) X(CmdClearColorImage)

#define PFN_FIELD(name) PFN_vk##name name;

struct instance {
	VkInstance handle;
	uint32_t api_version;
	PFN_vkGetInstanceProcAddr gipa;
	INSTANCE_FUNCS(PFN_FIELD)
};

/* Copied pNext node. */
union chain_node {
	VkBaseOutStructure base;
	uint64_t align;
	unsigned char bytes[NODE_BYTES];
};

/* Copyable chain struct size. */
struct chain_kind {
	VkStructureType type;
	size_t size;
};

struct known_queue {
	VkQueue queue;
	uint32_t family;
};

/* States shaded per pixel. */
enum native_state {
	NATIVE_SHADER_MASK  = 1 << 0,   /* reading gl_SampleMaskIn */
	NATIVE_SAMPLE_MASK  = 1 << 1,   /* partial pSampleMask */
	NATIVE_CONSERVATIVE = 1 << 2,
	NATIVE_LOCATIONS    = 1 << 3,   /* custom sample locations */
	NATIVE_DISCARD      = 1 << 4,   /* no MIN combiner */
};

/* Where coarse shading stops. */
struct rate_limits {
	uint32_t native;            /* enum native_state bits */
	VkSampleCountFlags samples; /* max coarse samples */
};

struct device {
	VkDevice handle;
	VkPhysicalDevice phys;
	struct instance *inst;
	PFN_vkGetDeviceProcAddr gdpa;
	PFN_vkSetDeviceLoaderData set_loader_data;
	struct {
		DEVICE_FUNCS(PFN_FIELD)
		PASS_FUNCS(PFN_FIELD)
	} vk;
	int vrs;
	_Atomic int unsafe;         /* unpatched pipeline exists */
	_Atomic int bind_rate;      /* dynamic-rate pipeline seen */
	struct passes *passes;      /* render pass twins, facts */
	VkExtent2D texel;
	struct rate_fit fit;
	struct rate_limits limits;
	VkPhysicalDeviceMemoryProperties memory;
	VkQueueFamilyProperties families[FAMILIES_MAX];
	uint32_t nfamilies;
	VkExtent2D max_framebuffer;
	struct known_queue queues[QUEUES_MAX];
	_Atomic uint32_t nqueues;
	pthread_mutex_t lock;
	_Atomic(VkQueue) present;   /* rate upload queue */
	_Atomic int present_failed;
	_Atomic int split;          /* scenes off present queue */
	_Atomic uint64_t swap_extent;
	struct fovea fovea;
	_Atomic uint64_t frames;
};

/* Thread's last device lookup. */
struct device_hit {
	const void *key;
	struct device *dev;
	uint64_t epoch;
};

/* Device patch staged for vkCreateDevice. */
struct device_setup {
	struct instance *inst;
	VkPhysicalDevice phys;
	const VkDeviceCreateInfo *app;
	VkDeviceCreateInfo patched;
	VkPhysicalDeviceFragmentShadingRateFeaturesKHR features;
	const char **extensions;
	VkExtent2D texel;
	struct rate_fit fit;
	struct rate_limits limits;
};

static inline uint64_t
extent_key(VkExtent2D e)
{
	return (uint64_t)e.width << EXTENT_SHIFT | e.height;
}

static inline VkExtent2D
extent_of(uint64_t key)
{
	return (VkExtent2D){ (uint32_t)(key >> EXTENT_SHIFT), (uint32_t)key };
}

static inline const VkBaseInStructure *
chain_find(const void *chain, VkStructureType type)
{
	const VkBaseInStructure *s = chain;

	while (s && s->sType != type)
		s = s->pNext;
	return s;
}

size_t chain_size(const struct chain_kind *kinds, size_t count, VkStructureType type);

int registry_add(const void *handle, void *object);
void registry_remove(const void *handle);
void *registry_find(const void *handle);

struct instance *instance_of(const void *handle);
VKAPI_ATTR VkResult VKAPI_CALL instance_create(const VkInstanceCreateInfo *requested,
	const VkAllocationCallbacks *alloc, VkInstance *out);
VKAPI_ATTR void VKAPI_CALL instance_destroy(VkInstance instance,
	const VkAllocationCallbacks *alloc);

int features_prepare(struct device_setup *s);
void features_release(struct device_setup *s);

extern PER_THREAD struct device_hit device_hit;
extern _Atomic uint64_t device_epoch;     /* bumped per destroyed device */

struct device *device_find(const void *handle);

/* Hits skip the registry. */
static inline struct device *
device_of(const void *handle)
{
	const struct device_hit *h = &device_hit;

	if (h->key != *(const void *const *)handle
			|| h->epoch != atomic_load_explicit(&device_epoch, memory_order_relaxed))
		return device_find(handle);
	return h->dev;
}

uint32_t device_family_of(struct device *dev, VkQueue queue);
VKAPI_ATTR VkResult VKAPI_CALL device_create(VkPhysicalDevice phys,
	const VkDeviceCreateInfo *requested, const VkAllocationCallbacks *alloc,
	VkDevice *out);
VKAPI_ATTR void VKAPI_CALL device_destroy(VkDevice device,
	const VkAllocationCallbacks *alloc);
VKAPI_ATTR void VKAPI_CALL device_get_queue(VkDevice device, uint32_t family,
	uint32_t index, VkQueue *out);
VKAPI_ATTR void VKAPI_CALL device_get_queue2(VkDevice device,
	const VkDeviceQueueInfo2 *request, VkQueue *out);

VKAPI_ATTR VkResult VKAPI_CALL frame_present(VkQueue queue,
	const VkPresentInfoKHR *present);
VKAPI_ATTR VkResult VKAPI_CALL swap_create(VkDevice device,
	const VkSwapchainCreateInfoKHR *create, const VkAllocationCallbacks *alloc,
	VkSwapchainKHR *out);

VKAPI_ATTR VkResult VKAPI_CALL submit_queue(VkQueue queue, uint32_t count,
	const VkSubmitInfo *submits, VkFence fence);
VKAPI_ATTR VkResult VKAPI_CALL submit_queue2(VkQueue queue, uint32_t count,
	const VkSubmitInfo2 *submits, VkFence fence);

void link_connect(void);
void link_check(void);
struct vrs_link *link_page(void);

#endif
