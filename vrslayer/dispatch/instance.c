#include "layer.h"

#include <stdlib.h>

#define NEEDED_API VK_API_VERSION_1_2   /* render pass 2, FSR */
#define RAISED_API VK_API_VERSION_1_3

static VkLayerInstanceCreateInfo *
loader_link(const VkInstanceCreateInfo *requested)
{
	const VkLayerInstanceCreateInfo *l = requested->pNext;

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
	const VkInstanceCreateInfo *raised)
{
	struct instance *inst = calloc(1, sizeof(*inst));

	if (!inst)
		return NULL;
	inst->handle = handle;
	inst->gipa = gipa;
	inst->api_version = raised->pApplicationInfo
		? raised->pApplicationInfo->apiVersion : VK_API_VERSION_1_0;
#define X(name) inst->name = (PFN_vk##name)gipa(handle, "vk" #name);
	INSTANCE_FUNCS(X)
#undef X
	return inst;
}

/* Drivers clamp; never lowered. */
static uint32_t
raised_api(uint32_t app)
{
	return app >= NEEDED_API ? app : RAISED_API;
}

VKAPI_ATTR VkResult VKAPI_CALL
instance_create(const VkInstanceCreateInfo *requested,
	const VkAllocationCallbacks *alloc, VkInstance *out)
{
	VkLayerInstanceCreateInfo *chain = loader_link(requested);
	VkInstanceCreateInfo raised = *requested;
	VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO };
	PFN_vkGetInstanceProcAddr gipa;
	PFN_vkCreateInstance create;
	struct instance *inst;
	VkResult created;

	if (!chain)
		return VK_ERROR_INITIALIZATION_FAILED;
	gipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
	chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;
	if (requested->pApplicationInfo)
		app = *requested->pApplicationInfo;
	app.apiVersion = raised_api(app.apiVersion);
	raised.pApplicationInfo = &app;
	create = (PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance");
	created = create(&raised, alloc, out);
	if (created != VK_SUCCESS)
		return created;

	inst = instance_new(*out, gipa, &raised);
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
