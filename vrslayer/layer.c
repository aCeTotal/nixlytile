#include "layer.h"

#include <string.h>

#define LENGTH(a) (sizeof(a) / sizeof((a)[0]))
#define HOOK(name, fn) { "vk" #name, (PFN_vkVoidFunction)(fn) }
#define LOADER_INTERFACE 2

struct hook {
	const char *name;
	PFN_vkVoidFunction fn;
};

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL layer_gipa(VkInstance instance,
	const char *name);
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL layer_gdpa(VkDevice device,
	const char *name);

static const struct hook instance_hooks[] = {
	HOOK(GetInstanceProcAddr, layer_gipa),
	HOOK(GetDeviceProcAddr, layer_gdpa),
	HOOK(CreateInstance, instance_create),
	HOOK(DestroyInstance, instance_destroy),
	HOOK(CreateDevice, device_create),
};

static const struct hook device_hooks[] = {
	HOOK(GetDeviceProcAddr, layer_gdpa),
	HOOK(DestroyDevice, device_destroy),
	HOOK(GetDeviceQueue, device_get_queue),
	HOOK(GetDeviceQueue2, device_get_queue2),
	HOOK(CreateSwapchainKHR, frame_swapchain),
	HOOK(QueuePresentKHR, frame_present),
	HOOK(AcquireNextImageKHR, frame_acquire),
	HOOK(AcquireNextImage2KHR, frame_acquire2),
	HOOK(QueueSubmit, submit_queue),
	HOOK(QueueSubmit2, submit_queue2),
	HOOK(QueueSubmit2KHR, submit_queue2),
	HOOK(CmdBeginRendering, rendering_begin),
	HOOK(CmdBeginRenderingKHR, rendering_begin),
	HOOK(CreateGraphicsPipelines, pipeline_create_graphics),
	HOOK(BeginCommandBuffer, drawrate_begin),
	HOOK(CmdBindPipeline, drawrate_bind),
	HOOK(CmdSetDepthTestEnable, drawrate_depth_test),
	HOOK(CmdSetDepthTestEnableEXT, drawrate_depth_test),
	HOOK(CmdSetDepthCompareOp, drawrate_depth_compare),
	HOOK(CmdSetDepthCompareOpEXT, drawrate_depth_compare),
};

static PFN_vkVoidFunction
lookup(const struct hook *hooks, size_t n, const char *name)
{
	size_t i;

	for (i = 0; i < n; i++)
		if (strcmp(hooks[i].name, name) == 0)
			return hooks[i].fn;
	return NULL;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
layer_gipa(VkInstance instance, const char *name)
{
	PFN_vkVoidFunction hook = lookup(instance_hooks, LENGTH(instance_hooks), name);
	struct instance *inst;

	if (hook || !instance)
		return hook;
	inst = instance_of(instance);
	return inst ? inst->gipa(instance, name) : NULL;
}

/* Hooked only if driver supports. */
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
layer_gdpa(VkDevice device, const char *name)
{
	struct device *dev = device_of(device);
	PFN_vkVoidFunction next = dev->gdpa(device, name);
	PFN_vkVoidFunction hook = lookup(device_hooks, LENGTH(device_hooks), name);

	return hook && next ? hook : next;
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *v)
{
	if (v->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT
			|| v->loaderLayerInterfaceVersion < LOADER_INTERFACE)
		return VK_ERROR_INITIALIZATION_FAILED;
	v->loaderLayerInterfaceVersion = LOADER_INTERFACE;
	v->pfnGetInstanceProcAddr = layer_gipa;
	v->pfnGetDeviceProcAddr = layer_gdpa;
	v->pfnGetPhysicalDeviceProcAddr = NULL;
	return VK_SUCCESS;
}

EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance instance, const char *name)
{
	return layer_gipa(instance, name);
}

EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetDeviceProcAddr(VkDevice device, const char *name)
{
	return layer_gdpa(device, name);
}
