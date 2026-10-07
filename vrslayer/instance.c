#include "layer.h"

#include <stdlib.h>

static VkLayerInstanceCreateInfo *
link_info(const VkInstanceCreateInfo *info)
{
	const VkLayerInstanceCreateInfo *l = info->pNext;

	while (l && !(l->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO
			&& l->function == VK_LAYER_LINK_INFO))
		l = l->pNext;
	return (VkLayerInstanceCreateInfo *)l;
}

struct instance *
instance_of(const void *handle)
{
	return registry_find(handle);
}

static struct instance *
instance_new(VkInstance handle, PFN_vkGetInstanceProcAddr gipa,
	const VkInstanceCreateInfo *info)
{
	struct instance *inst = calloc(1, sizeof(*inst));

	if (!inst)
		return NULL;
	inst->handle = handle;
	inst->gipa = gipa;
	inst->api_version = info->pApplicationInfo
		? info->pApplicationInfo->apiVersion : VK_API_VERSION_1_0;
#define X(name) inst->name = (PFN_vk##name)gipa(handle, "vk" #name);
	INSTANCE_FUNCS(X)
#undef X
	return inst;
}

VKAPI_ATTR VkResult VKAPI_CALL
instance_create(const VkInstanceCreateInfo *info,
	const VkAllocationCallbacks *alloc, VkInstance *out)
{
	VkLayerInstanceCreateInfo *chain = link_info(info);
	PFN_vkGetInstanceProcAddr gipa;
	PFN_vkCreateInstance create;
	struct instance *inst;
	VkResult res;

	if (!chain)
		return VK_ERROR_INITIALIZATION_FAILED;
	gipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
	chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;
	create = (PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance");
	res = create(info, alloc, out);
	if (res != VK_SUCCESS)
		return res;

	inst = instance_new(*out, gipa, info);
	if (inst && registry_add(*out, inst))
		return VK_SUCCESS;
	free(inst);
	((PFN_vkDestroyInstance)gipa(*out, "vkDestroyInstance"))(*out, alloc);
	return VK_ERROR_OUT_OF_HOST_MEMORY;
}

VKAPI_ATTR void VKAPI_CALL
instance_destroy(VkInstance instance, const VkAllocationCallbacks *alloc)
{
	struct instance *inst;
	PFN_vkDestroyInstance destroy;

	if (!instance)
		return;
	inst = instance_of(instance);
	destroy = inst->DestroyInstance;
	registry_remove(instance);
	free(inst);
	destroy(instance, alloc);
}
