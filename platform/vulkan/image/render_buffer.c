#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include "platform/buffer/buffer.h"
#include "platform/log/log.h"
#include "platform/vulkan/image/render_buffer.h"
#include "platform/vulkan/image/texture.h"
#include "platform/vulkan/pipeline/descriptor_pool.h"
#include "platform/vulkan/renderer.h"

static void free_render_buffer(struct nx_vk_render_buffer *buffer) {
	VkDevice dev = buffer->renderer->dev->dev;
	for (size_t i = 0; i < NX_VK_PATHWAY_COUNT; i++) {
		vkDestroyFramebuffer(dev, buffer->targets[i].framebuffer, NULL);
		vkDestroyImageView(dev, buffer->targets[i].image_view, NULL);
	}

	vkDestroyImage(dev, buffer->blend.image, NULL);
	vkFreeMemory(dev, buffer->blend.memory, NULL);
	vkDestroyImageView(dev, buffer->blend.image_view, NULL);
	if (buffer->blend.ds_pool) {
		vulkan_free_ds(buffer->renderer, buffer->blend.ds_pool, buffer->blend.ds);
	}

	vkDestroyImage(dev, buffer->image, NULL);
	for (uint32_t i = 0; i < buffer->mem_count; ++i) {
		vkFreeMemory(dev, buffer->memories[i], NULL);
	}
	free(buffer);
}

static bool gpu_is_using(const struct nx_vk_render_buffer *buffer) {
	if (buffer->last_timeline_point == 0) {
		return false;
	}
	struct nx_vk_renderer *renderer = buffer->renderer;
	uint64_t current_point;
	VkResult res = renderer->dev->api.vkGetSemaphoreCounterValueKHR(renderer->dev->dev,
		renderer->timeline_semaphore, &current_point);
	return res == VK_SUCCESS && current_point < buffer->last_timeline_point;
}

// Deferred until the GPU finishes.
static void destroy_render_buffer(struct nx_vk_render_buffer *buffer) {
	wl_list_remove(&buffer->link);
	nx_addon_finish(&buffer->addon);

	if (gpu_is_using(buffer)) {
		wl_list_insert(&buffer->renderer->pending_destroy_render_buffers,
			&buffer->pending_destroy_link);
		return;
	}
	free_render_buffer(buffer);
}

static void handle_render_buffer_destroy(struct nx_addon *addon) {
	struct nx_vk_render_buffer *buffer = wl_container_of(addon, buffer, addon);
	destroy_render_buffer(buffer);
}

static const struct nx_addon_interface render_buffer_addon_impl = {
	.name = "nx_vk_render_buffer",
	.destroy = handle_render_buffer_destroy,
};

static bool import_render_image(struct nx_vk_render_buffer *buffer,
		const struct nx_dmabuf_attributes *dmabuf) {
	struct nx_vk_dmabuf_image imported;
	if (!vulkan_import_dmabuf(buffer->renderer, dmabuf, NX_VK_MODIFIER_RENDER, &imported)) {
		return false;
	}
	buffer->image = imported.image;
	buffer->mem_count = imported.mem_count;
	memcpy(buffer->memories, imported.memories, sizeof(buffer->memories));

	if (imported.has_mutable_srgb) {
		return vulkan_setup_one_pass_framebuffer(buffer, dmabuf, NX_VK_PATHWAY_SRGB);
	}
	return vulkan_setup_two_pass_framebuffer(buffer, dmabuf);
}

static struct nx_vk_render_buffer *create_render_buffer(struct nx_vk_renderer *renderer,
		struct nx_buffer *nx_buffer) {
	struct nx_vk_render_buffer *buffer = calloc(1, sizeof(*buffer));
	if (buffer == NULL) {
		nx_log_errno(NX_ERROR, "Allocation failed");
		return NULL;
	}
	buffer->nx_buffer = nx_buffer;
	buffer->renderer = renderer;
	wl_list_init(&buffer->pending_destroy_link);
	nx_addon_init(&buffer->addon, &nx_buffer->addons, renderer, &render_buffer_addon_impl);
	wl_list_insert(&renderer->render_buffers, &buffer->link);

	struct nx_dmabuf_attributes dmabuf = {0};
	if (!nx_buffer_get_dmabuf(nx_buffer, &dmabuf)) {
		goto error;
	}

	nx_log(NX_DEBUG, "vulkan create_render_buffer: %.4s, %dx%d",
		(const char *)&dmabuf.format, dmabuf.width, dmabuf.height);

	if (!import_render_image(buffer, &dmabuf)) {
		goto error;
	}
	return buffer;

error:
	destroy_render_buffer(buffer);
	nx_dmabuf_attributes_finish(&dmabuf);
	return NULL;
}

struct nx_vk_render_buffer *vulkan_get_or_create_render_buffer(
		struct nx_vk_renderer *renderer, struct nx_buffer *nx_buffer) {
	struct nx_addon *addon =
		nx_addon_find(&nx_buffer->addons, renderer, &render_buffer_addon_impl);
	if (addon == NULL) {
		return create_render_buffer(renderer, nx_buffer);
	}
	struct nx_vk_render_buffer *buffer = wl_container_of(addon, buffer, addon);
	return buffer;
}

void vulkan_render_buffers_reap(struct nx_vk_renderer *renderer, uint64_t current_point) {
	struct nx_vk_render_buffer *buffer, *tmp;
	wl_list_for_each_safe(buffer, tmp, &renderer->pending_destroy_render_buffers,
			pending_destroy_link) {
		if (buffer->last_timeline_point <= current_point) {
			wl_list_remove(&buffer->pending_destroy_link);
			free_render_buffer(buffer);
		}
	}
}

// The device is idle.
void vulkan_render_buffers_finish(struct nx_vk_renderer *renderer) {
	struct nx_vk_render_buffer *buffer, *tmp;
	wl_list_for_each_safe(buffer, tmp, &renderer->render_buffers, link) {
		destroy_render_buffer(buffer);
	}
	wl_list_for_each_safe(buffer, tmp, &renderer->pending_destroy_render_buffers,
			pending_destroy_link) {
		wl_list_remove(&buffer->pending_destroy_link);
		free_render_buffer(buffer);
	}
}
