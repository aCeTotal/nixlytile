#include "passes.h"

#include <stdlib.h>

#define FRAGMENT  VK_SHADER_STAGE_FRAGMENT_BIT
#define RATE_FLAG VK_SHADER_CREATE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_EXT

static const uint32_t object_of[] = {
	[SPIRV_SAFE] = 0, [SPIRV_DISCARD] = OBJECT_DISCARD, [SPIRV_UNSAFE] = OBJECT_UNSAFE,
};

/* Unsafe, discarding, flagged fragments only. */
static void
remember(struct device *dev, const VkShaderCreateInfoEXT *in, VkShaderEXT shader)
{
	uint32_t verdict;

	if (!shader || in->stage != FRAGMENT)
		return;
	verdict = object_of[shader_object_verdict(dev, in)]
		| (in->flags & RATE_FLAG ? OBJECT_OWN : 0);
	if (verdict && !handlemap_put(&dev->passes->objects, (uint64_t)shader, verdict))
		atomic_store(&dev->passes->blind, 1);
}

/* Fragment shaders meet our attachment. */
VKAPI_ATTR VkResult VKAPI_CALL
shaderobject_create(VkDevice device, uint32_t count, const VkShaderCreateInfoEXT *creates,
	const VkAllocationCallbacks *alloc, VkShaderEXT *out)
{
	struct device *dev = device_of(device);
	VkShaderCreateInfoEXT *flagged = malloc(count * sizeof(*flagged));
	VkResult created;
	uint32_t i;

	atomic_store(&dev->bind_rate, 1);
	if (!flagged) {
		atomic_store(&dev->passes->blind, 1);
		return dev->vk.CreateShadersEXT(device, count, creates, alloc, out);
	}
	for (i = 0; i < count; i++) {
		flagged[i] = creates[i];
		flagged[i].flags |= creates[i].stage == FRAGMENT ? RATE_FLAG : 0;
	}
	created = dev->vk.CreateShadersEXT(device, count, flagged, alloc, out);
	free(flagged);
	for (i = 0; i < count; i++)
		remember(dev, &creates[i], out[i]);
	return created;
}

VKAPI_ATTR void VKAPI_CALL
shaderobject_destroy(VkDevice device, VkShaderEXT shader,
	const VkAllocationCallbacks *alloc)
{
	struct device *dev = device_of(device);

	if (shader)
		handlemap_take(&dev->passes->objects, (uint64_t)shader);
	dev->vk.DestroyShaderEXT(device, shader, alloc);
}

static unsigned char
verdict_of(struct device *dev, VkShaderEXT shader)
{
	return shader ? (unsigned char)handlemap_get(&dev->passes->objects, (uint64_t)shader) : 0;
}

/* All dynamic; fragment decides. */
static void
learn_objects(struct device *dev, struct recording *r)
{
	int keep = (r->fragment & OBJECT_UNSAFE) || perpixel_objects(dev)
		|| atomic_load_explicit(&dev->passes->blind, memory_order_relaxed);

	r->mode = keep ? MODE_KEEP : MODE_DYNAMIC;
	r->dynamic = 1;
	r->layer = 0;
	r->held = 0;
	r->own = (r->fragment & OBJECT_OWN) != 0;
	r->discards = (r->fragment & OBJECT_DISCARD) != 0;
}

/* Graphics stages unbind the pipeline. */
VKAPI_ATTR void VKAPI_CALL
shaderobject_bind(VkCommandBuffer cmd, uint32_t count,
	const VkShaderStageFlagBits *stages, const VkShaderEXT *shaders)
{
	struct device *dev = device_of(cmd);
	struct recording *r = recording_of(dev, cmd);
	int graphics = 0;
	uint32_t i;

	dev->vk.CmdBindShadersEXT(cmd, count, stages, shaders);
	for (i = 0; i < count; i++) {
		graphics |= stages[i] != VK_SHADER_STAGE_COMPUTE_BIT;
		if (stages[i] == FRAGMENT)
			r->fragment = verdict_of(dev, shaders ? shaders[i] : VK_NULL_HANDLE);
	}
	if (!graphics)
		return;
	learn_objects(dev, r);
	drawrate_apply(dev, cmd, r);
}
