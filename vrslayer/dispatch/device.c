#include "passes.h"

#include <stdlib.h>

/* Core name, extension suffix. */
#define ALIASES(X) \
	X(CreateRenderPass2, KHR) X(CmdBeginRenderPass2, KHR) \
	X(CmdBeginRendering, KHR) X(CmdSetDepthTestEnable, EXT) \
	X(CmdSetDepthCompareOp, EXT) X(QueueSubmit2, KHR) \
	X(CmdSetViewportWithCount, EXT)

static VkLayerDeviceCreateInfo *
loader_link(const VkDeviceCreateInfo *requested, VkLayerFunction function)
{
	const VkLayerDeviceCreateInfo *l = requested->pNext;

	while (l && !(l->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO
			&& l->function == function))
		l = l->pNext;
	return (VkLayerDeviceCreateInfo *)l;
}

PER_THREAD struct device_hit device_hit;
_Atomic uint64_t device_epoch;

/* Epoch first; absence never cached. */
struct device *
device_find(const void *handle)
{
	uint64_t epoch = atomic_load_explicit(&device_epoch, memory_order_relaxed);
	struct device *dev = registry_find(handle);

	if (dev)
		device_hit = (struct device_hit){ *(const void *const *)handle, dev, epoch };
	return dev;
}

uint32_t
device_family_of(struct device *dev, VkQueue queue)
{
	uint32_t i = 0, n = atomic_load(&dev->nqueues);

	while (i < n && dev->queues[i].queue != queue)
		i++;
	return i < n ? dev->queues[i].family : UINT32_MAX;
}

static void
load_dispatch(struct device *dev)
{
	PFN_vkGetDeviceProcAddr gdpa = dev->gdpa;

#define X(name) dev->vk.name = (PFN_vk##name)gdpa(dev->handle, "vk" #name);
	DEVICE_FUNCS(X)
	PASS_FUNCS(X)
#undef X
#define X(name, suffix) if (!dev->vk.name) \
	dev->vk.name = (PFN_vk##name)gdpa(dev->handle, "vk" #name #suffix);
	ALIASES(X)
#undef X
}

static void
load_properties(struct device *dev)
{
	VkPhysicalDeviceProperties2 props = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
	};
	uint32_t n = FAMILIES_MAX;

	dev->inst->GetPhysicalDeviceMemoryProperties(dev->phys, &dev->memory);
	dev->inst->GetPhysicalDeviceQueueFamilyProperties(dev->phys, &n,
		dev->families);
	dev->nfamilies = n;
	if (dev->inst->GetPhysicalDeviceProperties2)
		dev->inst->GetPhysicalDeviceProperties2(dev->phys, &props);
	dev->max_framebuffer = (VkExtent2D){ props.properties.limits.maxFramebufferWidth,
		props.properties.limits.maxFramebufferHeight };
}

static struct device *
device_new(VkDevice handle, const struct device_setup *s, int vrs)
{
	struct device *dev = calloc(1, sizeof(*dev));

	if (!dev)
		return NULL;
	dev->handle = handle;
	dev->phys = s->phys;
	dev->inst = s->inst;
	dev->vrs = vrs;
	dev->texel = s->texel;
	dev->fit = s->fit;
	dev->limits = s->limits;
	pthread_mutex_init(&dev->lock, NULL);
	return dev;
}

/* Patch failure retries unpatched. */
VKAPI_ATTR VkResult VKAPI_CALL
device_create(VkPhysicalDevice phys, const VkDeviceCreateInfo *requested,
	const VkAllocationCallbacks *alloc, VkDevice *out)
{
	VkLayerDeviceCreateInfo *chain = loader_link(requested, VK_LAYER_LINK_INFO);
	VkLayerDeviceCreateInfo *loader = loader_link(requested, VK_LOADER_DATA_CALLBACK);
	struct device_setup setup = { .inst = instance_of(phys), .phys = phys,
		.app = requested };
	PFN_vkGetDeviceProcAddr gdpa;
	PFN_vkCreateDevice create;
	VkLayerDeviceLink *next;
	struct device *dev;
	VkResult created;
	int vrs;

	if (!chain || !loader || !setup.inst)
		return VK_ERROR_INITIALIZATION_FAILED;
	gdpa = chain->u.pLayerInfo->pfnNextGetDeviceProcAddr;
	create = (PFN_vkCreateDevice)chain->u.pLayerInfo->pfnNextGetInstanceProcAddr(
		setup.inst->handle, "vkCreateDevice");
	next = chain->u.pLayerInfo->pNext;

	vrs = features_prepare(&setup);
	chain->u.pLayerInfo = next;
	created = create(phys, vrs ? &setup.patched : requested, alloc, out);
	if (created != VK_SUCCESS && vrs) {
		vrs = 0;
		chain->u.pLayerInfo = next;
		created = create(phys, requested, alloc, out);
	}
	features_release(&setup);
	if (created != VK_SUCCESS)
		return created;

	dev = device_new(*out, &setup, vrs);
	if (!dev || !registry_add(*out, dev)) {
		((PFN_vkDestroyDevice)gdpa(*out, "vkDestroyDevice"))(*out, alloc);
		free(dev);
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}
	dev->gdpa = gdpa;
	dev->set_loader_data = loader->u.pfnSetDeviceLoaderData;
	load_dispatch(dev);
	load_properties(dev);
	if (dev->vrs && !passes_create(dev, requested))
		dev->vrs = 0;
	worker_start(dev);
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
device_destroy(VkDevice device, const VkAllocationCallbacks *alloc)
{
	struct device *dev;

	if (!device)
		return;
	dev = device_of(device);
	worker_stop(dev);
	dev->vk.DeviceWaitIdle(device);
	upload_destroy(dev);
	passes_destroy(dev);
	rate_destroy(dev);
	atomic_fetch_add(&device_epoch, 1);
	registry_remove(device);
	dev->vk.DestroyDevice(device, alloc);
	pthread_mutex_destroy(&dev->lock);
	free(dev);
}

static void
remember_queue(struct device *dev, VkQueue queue, uint32_t family)
{
	uint32_t n;

	pthread_mutex_lock(&dev->lock);
	n = atomic_load(&dev->nqueues);
	if (n < QUEUES_MAX && device_family_of(dev, queue) == UINT32_MAX) {
		dev->queues[n] = (struct known_queue){ queue, family };
		atomic_store(&dev->nqueues, n + 1);
	}
	pthread_mutex_unlock(&dev->lock);
}

VKAPI_ATTR void VKAPI_CALL
device_get_queue(VkDevice device, uint32_t family, uint32_t index, VkQueue *out)
{
	struct device *dev = device_of(device);

	dev->vk.GetDeviceQueue(device, family, index, out);
	remember_queue(dev, *out, family);
}

VKAPI_ATTR void VKAPI_CALL
device_get_queue2(VkDevice device, const VkDeviceQueueInfo2 *request, VkQueue *out)
{
	struct device *dev = device_of(device);

	dev->vk.GetDeviceQueue2(device, request, out);
	if (*out)
		remember_queue(dev, *out, request->queueFamilyIndex);
}
