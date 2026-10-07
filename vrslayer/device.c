#include "layer.h"

#include <stdlib.h>

#define NOMINAL_W 128
#define NOMINAL_H 72

static VkLayerDeviceCreateInfo *
link_info(const VkDeviceCreateInfo *info, VkLayerFunction function)
{
	const VkLayerDeviceCreateInfo *l = info->pNext;

	while (l && !(l->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO
			&& l->function == function))
		l = l->pNext;
	return (VkLayerDeviceCreateInfo *)l;
}

struct device *
device_of(const void *handle)
{
	return registry_find(handle);
}

uint32_t
device_family_of(struct device *dev, VkQueue queue)
{
	uint32_t i, n = atomic_load(&dev->nqueues);

	for (i = 0; i < n; i++)
		if (dev->queues[i].queue == queue)
			return dev->queues[i].family;
	return UINT32_MAX;
}

uint32_t
device_memory_type(const struct device *dev, uint32_t bits,
	VkMemoryPropertyFlags flags)
{
	uint32_t i;

	for (i = 0; i < dev->memory.memoryTypeCount; i++)
		if ((bits & (1u << i)) && (dev->memory.memoryTypes[i].propertyFlags
				& flags) == flags)
			return i;
	return UINT32_MAX;
}

VkDeviceMemory
device_allocate(struct device *dev, VkMemoryRequirements req,
	VkMemoryPropertyFlags flags)
{
	VkMemoryAllocateInfo info = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = req.size,
		.memoryTypeIndex = device_memory_type(dev, req.memoryTypeBits, flags),
	};
	VkDeviceMemory memory = VK_NULL_HANDLE;

	if (info.memoryTypeIndex == UINT32_MAX)
		info.memoryTypeIndex = device_memory_type(dev, req.memoryTypeBits, 0);
	if (dev->vk.AllocateMemory(dev->handle, &info, NULL, &memory) != VK_SUCCESS)
		return VK_NULL_HANDLE;
	return memory;
}

static void
load_dispatch(struct device *dev)
{
	PFN_vkGetDeviceProcAddr gdpa = dev->gdpa;

#define X(name) dev->vk.name = (PFN_vk##name)gdpa(dev->handle, "vk" #name);
	DEVICE_FUNCS(X)
#undef X
	if (!dev->vk.CmdBeginRendering)
		dev->vk.CmdBeginRendering = (PFN_vkCmdBeginRendering)
			gdpa(dev->handle, "vkCmdBeginRenderingKHR");
	if (!dev->vk.CmdSetDepthTestEnable)
		dev->vk.CmdSetDepthTestEnable = (PFN_vkCmdSetDepthTestEnable)
			gdpa(dev->handle, "vkCmdSetDepthTestEnableEXT");
	if (!dev->vk.CmdSetDepthCompareOp)
		dev->vk.CmdSetDepthCompareOp = (PFN_vkCmdSetDepthCompareOp)
			gdpa(dev->handle, "vkCmdSetDepthCompareOpEXT");
	if (!dev->vk.QueueSubmit2)
		dev->vk.QueueSubmit2 = (PFN_vkQueueSubmit2)
			gdpa(dev->handle, "vkQueueSubmit2KHR");
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
	dev->timestamp_period = props.properties.limits.timestampPeriod;
}

/* Shaded fraction per level, 16:9. */
static void
load_costs(struct device *dev)
{
	uint8_t texels[NOMINAL_W * NOMINAL_H];
	struct pattern_grid g = {
		texels, NOMINAL_W, NOMINAL_H, 1, 1, NOMINAL_W, NOMINAL_H,
	};
	struct pattern p = {
		.codes = dev->codes,
		.foci = 1,
		.focus[0] = { 0.5f * NOMINAL_W, 0.5f * NOMINAL_H, 1.0f },
	};

	for (p.level = 0; p.level < VRS_LEVELS; p.level++)
		dev->control.cost[p.level] = pattern_cost(&g, &p);
}

static struct device *
device_new(VkDevice handle, const struct device_setup *s, int vrs)
{
	struct device *dev = calloc(1, sizeof(*dev));
	int i;

	if (!dev)
		return NULL;
	dev->handle = handle;
	dev->phys = s->phys;
	dev->inst = s->inst;
	dev->vrs = vrs;
	dev->texel = s->texel;
	for (i = 0; i < PATTERN_RINGS; i++)
		dev->codes[i] = s->codes[i];
	pthread_mutex_init(&dev->lock, NULL);
	return dev;
}

/* Patch failure retries unpatched. */
VKAPI_ATTR VkResult VKAPI_CALL
device_create(VkPhysicalDevice phys, const VkDeviceCreateInfo *info,
	const VkAllocationCallbacks *alloc, VkDevice *out)
{
	VkLayerDeviceCreateInfo *chain = link_info(info, VK_LAYER_LINK_INFO);
	VkLayerDeviceCreateInfo *loader = link_info(info, VK_LOADER_DATA_CALLBACK);
	struct device_setup setup = { .inst = instance_of(phys), .phys = phys, .app = info };
	PFN_vkGetDeviceProcAddr gdpa;
	PFN_vkCreateDevice create;
	VkLayerDeviceLink *next;
	struct device *dev;
	VkResult res;
	int vrs;

	if (!chain || !loader || !setup.inst)
		return VK_ERROR_INITIALIZATION_FAILED;
	gdpa = chain->u.pLayerInfo->pfnNextGetDeviceProcAddr;
	create = (PFN_vkCreateDevice)chain->u.pLayerInfo->pfnNextGetInstanceProcAddr(
		setup.inst->handle, "vkCreateDevice");
	next = chain->u.pLayerInfo->pNext;

	vrs = features_prepare(&setup);
	chain->u.pLayerInfo = next;
	res = create(phys, vrs ? &setup.info : info, alloc, out);
	if (res != VK_SUCCESS && vrs) {
		vrs = 0;
		chain->u.pLayerInfo = next;
		res = create(phys, info, alloc, out);
	}
	features_release(&setup);
	if (res != VK_SUCCESS)
		return res;

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
	load_costs(dev);
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
device_destroy(VkDevice device, const VkAllocationCallbacks *alloc)
{
	struct device *dev;

	if (!device)
		return;
	dev = device_of(device);
	dev->vk.DeviceWaitIdle(device);
	rate_destroy(dev);
	frame_destroy(dev);
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
		dev->queues[n] = (struct queue_info){ queue, family };
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
device_get_queue2(VkDevice device, const VkDeviceQueueInfo2 *info, VkQueue *out)
{
	struct device *dev = device_of(device);

	dev->vk.GetDeviceQueue2(device, info, out);
	if (*out)
		remember_queue(dev, *out, info->queueFamilyIndex);
}
