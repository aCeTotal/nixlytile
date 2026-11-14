#include <assert.h>
#include <stdlib.h>
#include <xf86drm.h>
#include <vulkan/vulkan.h>
#include "platform/log/log.h"
#include "platform/render/renderer_interface.h"
#include "platform/vulkan/image/render_buffer.h"
#include "platform/vulkan/image/texture.h"
#include "platform/vulkan/pass/pass.h"
#include "platform/vulkan/pipeline/pipeline.h"
#include "platform/vulkan/pipeline/pipeline_cache.h"
#include "platform/vulkan/renderer.h"
#include "platform/vulkan/vulkan.h"

static const struct nx_renderer_impl renderer_impl;

bool nx_renderer_is_vk(struct nx_renderer *nx_renderer) {
	return nx_renderer->impl == &renderer_impl;
}

struct nx_vk_renderer *vulkan_get_renderer(struct nx_renderer *nx_renderer) {
	assert(nx_renderer_is_vk(nx_renderer));
	struct nx_vk_renderer *renderer = wl_container_of(nx_renderer, renderer, nx_renderer);
	return renderer;
}

static const struct nx_drm_format_set *get_texture_formats(struct nx_renderer *nx_renderer,
		uint32_t buffer_caps) {
	struct nx_vk_renderer *renderer = vulkan_get_renderer(nx_renderer);
	if (buffer_caps & NX_BUFFER_CAP_DMABUF) {
		return &renderer->dev->dmabuf_formats[NX_VK_MODIFIER_TEXTURE];
	}
	if (buffer_caps & NX_BUFFER_CAP_DATA_PTR) {
		return &renderer->dev->shm_texture_formats;
	}
	return NULL;
}

static const struct nx_drm_format_set *get_render_formats(struct nx_renderer *nx_renderer) {
	struct nx_vk_renderer *renderer = vulkan_get_renderer(nx_renderer);
	return &renderer->dev->dmabuf_formats[NX_VK_MODIFIER_RENDER];
}

static int get_drm_fd(struct nx_renderer *nx_renderer) {
	struct nx_vk_renderer *renderer = vulkan_get_renderer(nx_renderer);
	return renderer->dev->drm_fd;
}

static struct nx_render_pass *begin_buffer_pass(struct nx_renderer *nx_renderer,
		struct nx_buffer *buffer, const struct nx_buffer_pass_options *options) {
	struct nx_vk_renderer *renderer = vulkan_get_renderer(nx_renderer);
	struct nx_vk_render_buffer *render_buffer =
		vulkan_get_or_create_render_buffer(renderer, buffer);
	if (!render_buffer) {
		return NULL;
	}

	struct nx_vk_render_pass *pass = vulkan_begin_render_pass(renderer, render_buffer, options);
	return pass ? &pass->base : NULL;
}

static const struct nx_renderer_impl renderer_impl = {
	.get_texture_formats = get_texture_formats,
	.get_render_formats = get_render_formats,
	.destroy = vulkan_renderer_destroy,
	.get_drm_fd = get_drm_fd,
	.texture_from_buffer = vulkan_texture_from_buffer,
	.begin_buffer_pass = begin_buffer_pass,
};

static void init_renderer(struct nx_vk_renderer *renderer, struct nx_vk_device *dev) {
	renderer->dev = dev;
	nx_renderer_init(&renderer->nx_renderer, &renderer_impl, NX_BUFFER_CAP_DMABUF);
	renderer->nx_renderer.features.input_color_transform = true;
	renderer->nx_renderer.features.output_color_transform = true;
	renderer->nx_renderer.color_encodings =
		NX_COLOR_ENCODING_BT601 | NX_COLOR_ENCODING_BT709 | NX_COLOR_ENCODING_BT2020;

	wl_list_init(&renderer->stage.buffers);
	wl_list_init(&renderer->foreign_textures);
	wl_list_init(&renderer->textures);
	vulkan_descriptor_pools_init(&renderer->texture_pools,
		VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
	vulkan_descriptor_pools_init(&renderer->blend_pools, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT);
	wl_list_init(&renderer->render_format_setups);
	wl_list_init(&renderer->render_buffers);
	wl_list_init(&renderer->pending_destroy_render_buffers);
	wl_list_init(&renderer->color_transforms);
	wl_list_init(&renderer->pipeline_layouts);
	wl_array_init(&renderer->submit.waits);
	wl_array_init(&renderer->submit.barriers);

	uint64_t cap_syncobj_timeline;
	if (dev->drm_fd >= 0 &&
			drmGetCap(dev->drm_fd, DRM_CAP_SYNCOBJ_TIMELINE, &cap_syncobj_timeline) == 0) {
		renderer->nx_renderer.features.timeline =
			dev->sync_file_import_export && cap_syncobj_timeline != 0;
	}
}

static bool create_command_pool(struct nx_vk_renderer *renderer) {
	VkCommandPoolCreateInfo cpool_info = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
		.queueFamilyIndex = renderer->dev->queue_family,
	};
	VkResult res = vkCreateCommandPool(renderer->dev->dev, &cpool_info, NULL,
		&renderer->command_pool);
	if (res != VK_SUCCESS) {
		nx_vk_error("vkCreateCommandPool", res);
		return false;
	}
	return true;
}

static bool create_timeline_semaphore(struct nx_vk_renderer *renderer) {
	VkSemaphoreTypeCreateInfoKHR semaphore_type_info = {
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO_KHR,
		.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE_KHR,
		.initialValue = 0,
	};
	VkSemaphoreCreateInfo semaphore_info = {
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
		.pNext = &semaphore_type_info,
	};
	VkResult res = vkCreateSemaphore(renderer->dev->dev, &semaphore_info, NULL,
		&renderer->timeline_semaphore);
	if (res != VK_SUCCESS) {
		nx_vk_error("vkCreateSemaphore", res);
		return false;
	}
	return true;
}

static struct nx_renderer *renderer_create_for_device(struct nx_vk_device *dev) {
	struct nx_vk_renderer *renderer = calloc(1, sizeof(*renderer));
	if (!renderer) {
		nx_log_errno(NX_ERROR, "failed to allocate nx_vk_renderer");
		return NULL;
	}
	init_renderer(renderer, dev);

	if (!vulkan_init_static_render_data(renderer) || !create_command_pool(renderer) ||
			!vulkan_pipeline_cache_init(renderer) ||
			!create_timeline_semaphore(renderer)) {
		vulkan_renderer_destroy(&renderer->nx_renderer);
		return NULL;
	}
	return &renderer->nx_renderer;
}

struct nx_renderer *nx_vk_renderer_create_with_drm_fd(int drm_fd) {
	struct nx_vk_instance *ini = vulkan_instance_create();
	if (!ini) {
		nx_log(NX_ERROR, "creating vulkan instance for renderer failed");
		return NULL;
	}

	VkPhysicalDevice phdev = vulkan_find_drm_phdev(ini, drm_fd);
	if (!phdev) {
		// Fail rather than guess.
		nx_log(NX_ERROR, "Could not match drm and vulkan device");
		vulkan_instance_destroy(ini);
		return NULL;
	}

	struct nx_vk_device *dev = vulkan_device_create(ini, phdev);
	if (!dev) {
		nx_log(NX_ERROR, "Failed to create vulkan device");
		vulkan_instance_destroy(ini);
		return NULL;
	}

	// Prefer the render node.
	dev->drm_fd = vulkan_open_phdev_drm_fd(phdev);
	return renderer_create_for_device(dev);
}
