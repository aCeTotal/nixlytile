#ifndef NX_VULKAN_PIPELINE_PIPELINE_CACHE_H
#define NX_VULKAN_PIPELINE_PIPELINE_CACHE_H

#include <stdbool.h>

struct nx_vk_renderer;

// Seeds the cache from disk.
bool vulkan_pipeline_cache_init(struct nx_vk_renderer *renderer);
// Background save when due.
void vulkan_pipeline_cache_tick(struct nx_vk_renderer *renderer);
void vulkan_pipeline_cache_save(struct nx_vk_renderer *renderer);

#endif
