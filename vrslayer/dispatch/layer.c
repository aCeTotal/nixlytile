#include "passes.h"

#include <string.h>

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
};

/* VRS devices only. */
static const struct hook vrs_hooks[] = {
	HOOK(GetDeviceQueue, device_get_queue),
	HOOK(GetDeviceQueue2, device_get_queue2),
	HOOK(CreateSwapchainKHR, swap_create),
	HOOK(QueuePresentKHR, frame_present),
	HOOK(QueueSubmit, submit_queue),
	HOOK(QueueSubmit2, submit_queue2),
	HOOK(QueueSubmit2KHR, submit_queue2),
	HOOK(CmdBeginRendering, rendering_begin),
	HOOK(CmdBeginRenderingKHR, rendering_begin),
	HOOK(CreateGraphicsPipelines, pipeline_create_graphics),
	HOOK(DestroyPipeline, pipeline_destroy),
	HOOK(CreateShaderModule, shader_create),
	HOOK(DestroyShaderModule, shader_destroy),
	HOOK(CreateShadersEXT, shaderobject_create),
	HOOK(DestroyShaderEXT, shaderobject_destroy),
	HOOK(CmdBindShadersEXT, shaderobject_bind),
	HOOK(CreateRenderPass, renderpass_create),
	HOOK(CreateRenderPass2, renderpass_create2),
	HOOK(CreateRenderPass2KHR, renderpass_create2),
	HOOK(DestroyRenderPass, renderpass_destroy),
	HOOK(CreateFramebuffer, framebuffer_create),
	HOOK(DestroyFramebuffer, framebuffer_destroy),
	HOOK(CmdBeginRenderPass, pass_begin),
	HOOK(CmdBeginRenderPass2, pass_begin2),
	HOOK(CmdBeginRenderPass2KHR, pass_begin2),
	HOOK(AllocateCommandBuffers, recording_allocate),
	HOOK(FreeCommandBuffers, recording_free),
	HOOK(DestroyCommandPool, recording_pool_destroy),
	HOOK(BeginCommandBuffer, drawrate_begin),
	HOOK(CmdBindPipeline, drawrate_bind),
	HOOK(CmdExecuteCommands, drawrate_execute),
	HOOK(CmdSetDepthTestEnable, drawrate_depth_test),
	HOOK(CmdSetDepthTestEnableEXT, drawrate_depth_test),
	HOOK(CmdSetDepthCompareOp, drawrate_depth_compare),
	HOOK(CmdSetDepthCompareOpEXT, drawrate_depth_compare),
	HOOK(CmdSetAlphaToCoverageEnableEXT, drawrate_coverage),
	HOOK(CmdSetFragmentShadingRateKHR, drawrate_app_rate),
	HOOK(CmdSetViewport, drawrate_viewport),
	HOOK(CmdSetViewportWithCount, drawrate_viewport_count),
	HOOK(CmdSetViewportWithCountEXT, drawrate_viewport_count),
};

static PFN_vkVoidFunction
lookup(const struct hook *hooks, size_t n, const char *name)
{
	size_t i = 0;

	while (i < n && strcmp(hooks[i].name, name) != 0)
		i++;
	return i < n ? hooks[i].fn : NULL;
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
	struct device *dev = device ? device_of(device) : NULL;
	PFN_vkVoidFunction next = dev ? dev->gdpa(device, name) : NULL;
	PFN_vkVoidFunction hook = lookup(device_hooks, LENGTH(device_hooks), name);

	if (!hook && next && dev->vrs)
		hook = lookup(vrs_hooks, LENGTH(vrs_hooks), name);
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
