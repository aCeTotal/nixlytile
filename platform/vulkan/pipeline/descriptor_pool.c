#include <stdlib.h>
#include <vulkan/vulkan.h>
#include "platform/log/log.h"
#include "platform/vulkan/pipeline/descriptor_pool.h"
#include "platform/vulkan/renderer.h"

#define START_POOL_SIZE 256u

void vulkan_descriptor_pools_init(struct nx_vk_descriptor_pools *pools,
		VkDescriptorType type) {
	wl_list_init(&pools->list);
	pools->last_size = 0;
	pools->type = type;
}

void vulkan_descriptor_pools_finish(struct nx_vk_descriptor_pools *pools, VkDevice dev) {
	struct nx_vk_descriptor_pool *pool, *tmp;
	wl_list_for_each_safe(pool, tmp, &pools->list, link) {
		vkDestroyDescriptorPool(dev, pool->pool, NULL);
		free(pool);
	}
}

// Each pool doubles the last.
static struct nx_vk_descriptor_pool *create_pool(VkDevice dev,
		struct nx_vk_descriptor_pools *pools) {
	struct nx_vk_descriptor_pool *pool = calloc(1, sizeof(*pool));
	if (!pool) {
		nx_log_errno(NX_ERROR, "allocation failed");
		return NULL;
	}

	size_t count = 2 * pools->last_size;
	if (!count) {
		count = START_POOL_SIZE;
	}
	pool->free = count;

	VkDescriptorPoolSize pool_size = {
		.descriptorCount = count,
		.type = pools->type,
	};
	VkDescriptorPoolCreateInfo dpool_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = count,
		.poolSizeCount = 1,
		.pPoolSizes = &pool_size,
		.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
	};
	VkResult res = vkCreateDescriptorPool(dev, &dpool_info, NULL, &pool->pool);
	if (res != VK_SUCCESS) {
		nx_vk_error("vkCreateDescriptorPool", res);
		free(pool);
		return NULL;
	}

	pools->last_size = count;
	wl_list_insert(&pools->list, &pool->link);
	return pool;
}

static struct nx_vk_descriptor_pool *alloc_ds(VkDevice dev,
		struct nx_vk_descriptor_pools *pools, VkDescriptorSetLayout layout,
		VkDescriptorSet *ds) {
	VkDescriptorSetAllocateInfo ds_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorSetCount = 1,
		.pSetLayouts = &layout,
	};

	struct nx_vk_descriptor_pool *pool;
	wl_list_for_each(pool, &pools->list, link) {
		if (pool->free == 0) {
			continue;
		}
		ds_info.descriptorPool = pool->pool;
		VkResult res = vkAllocateDescriptorSets(dev, &ds_info, ds);
		// Fragmented pool; try the next.
		if (res == VK_ERROR_FRAGMENTED_POOL || res == VK_ERROR_OUT_OF_POOL_MEMORY) {
			continue;
		}
		if (res != VK_SUCCESS) {
			nx_vk_error("vkAllocateDescriptorSets", res);
			return NULL;
		}
		--pool->free;
		return pool;
	}

	pool = create_pool(dev, pools);
	if (!pool) {
		return NULL;
	}
	ds_info.descriptorPool = pool->pool;
	VkResult res = vkAllocateDescriptorSets(dev, &ds_info, ds);
	if (res != VK_SUCCESS) {
		nx_vk_error("vkAllocateDescriptorSets", res);
		return NULL;
	}
	--pool->free;
	return pool;
}

struct nx_vk_descriptor_pool *vulkan_alloc_texture_ds(struct nx_vk_renderer *renderer,
		VkDescriptorSetLayout layout, VkDescriptorSet *ds) {
	return alloc_ds(renderer->dev->dev, &renderer->texture_pools, layout, ds);
}

struct nx_vk_descriptor_pool *vulkan_alloc_blend_ds(struct nx_vk_renderer *renderer,
		VkDescriptorSet *ds) {
	return alloc_ds(renderer->dev->dev, &renderer->blend_pools,
		renderer->output_ds_srgb_layout, ds);
}

void vulkan_free_ds(struct nx_vk_renderer *renderer, struct nx_vk_descriptor_pool *pool,
		VkDescriptorSet ds) {
	vkFreeDescriptorSets(renderer->dev->dev, pool->pool, 1, &ds);
	++pool->free;
}

void vulkan_write_sampler_descriptor(VkDevice dev, VkDescriptorSet ds, VkImageView view) {
	VkDescriptorImageInfo image_info = {
		.imageView = view,
		.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	};
	VkWriteDescriptorSet ds_write = {
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		.dstSet = ds,
		.pImageInfo = &image_info,
	};
	vkUpdateDescriptorSets(dev, 1, &ds_write, 0, NULL);
}
