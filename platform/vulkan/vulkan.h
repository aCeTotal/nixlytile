#ifndef NX_RENDER_VULKAN_H
#define NX_RENDER_VULKAN_H

#include <stdbool.h>
#include "platform/render/renderer.h"

struct nx_renderer *nx_vk_renderer_create_with_drm_fd(int drm_fd);
bool nx_renderer_is_vk(struct nx_renderer *nx_renderer);

#endif
