#ifndef NX_VULKAN_PIPELINE_DESCRIPTOR_POOL_H
#define NX_VULKAN_PIPELINE_DESCRIPTOR_POOL_H

#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan.h>
#include <wayland-util.h>

struct nx_vk_renderer;

struct nx_vk_descriptor_pool {
	VkDescriptorPool pool;
	uint32_t free; // sets still allocatable
	struct wl_list link; // nx_vk_descriptor_pools.list
};

// Growing pools, one descriptor type.
struct nx_vk_descriptor_pools {
	struct wl_list list; // nx_vk_descriptor_pool.link
	size_t last_size;
	VkDescriptorType type;
};

void vulkan_descriptor_pools_init(struct nx_vk_descriptor_pools *pools,
	VkDescriptorType type);
void vulkan_descriptor_pools_finish(struct nx_vk_descriptor_pools *pools, VkDevice dev);

// Returns the owning pool.
struct nx_vk_descriptor_pool *vulkan_alloc_texture_ds(struct nx_vk_renderer *renderer,
	VkDescriptorSetLayout layout, VkDescriptorSet *ds);
struct nx_vk_descriptor_pool *vulkan_alloc_blend_ds(struct nx_vk_renderer *renderer,
	VkDescriptorSet *ds);
void vulkan_free_ds(struct nx_vk_renderer *renderer, struct nx_vk_descriptor_pool *pool,
	VkDescriptorSet ds);
void vulkan_write_sampler_descriptor(VkDevice dev, VkDescriptorSet ds, VkImageView view);

#endif
