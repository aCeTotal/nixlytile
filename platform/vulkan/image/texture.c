#include <assert.h>
#include <stdlib.h>
#include <vulkan/vulkan.h>
#include "platform/buffer/buffer.h"
#include "platform/log/log.h"
#include "platform/render/pixel_format.h"
#include "platform/render/renderer_interface.h"
#include "platform/vulkan/image/read_pixels.h"
#include "platform/vulkan/image/texture.h"
#include "platform/vulkan/pipeline/descriptor_pool.h"
#include "platform/vulkan/renderer.h"

static const struct nx_texture_impl texture_impl;

static bool texture_is_vk(struct nx_texture *nx_texture) {
	return nx_texture->impl == &texture_impl;
}

struct nx_vk_texture *vulkan_get_texture(struct nx_texture *nx_texture) {
	assert(texture_is_vk(nx_texture));
	struct nx_vk_texture *texture = wl_container_of(nx_texture, texture, nx_texture);
	return texture;
}

void vulkan_texture_destroy(struct nx_vk_texture *texture) {
	if (texture->buffer != NULL) {
		nx_addon_finish(&texture->buffer_addon);
		texture->buffer = NULL;
	}

	// Freed after pending GPU work.
	if (texture->last_used_cb != NULL) {
		assert(texture->destroy_link.next == NULL);
		wl_list_insert(&texture->last_used_cb->destroy_textures, &texture->destroy_link);
		return;
	}

	wl_list_remove(&texture->link);

	VkDevice dev = texture->renderer->dev->dev;
	struct nx_vk_texture_view *view, *tmp_view;
	wl_list_for_each_safe(view, tmp_view, &texture->views, link) {
		vulkan_free_ds(texture->renderer, view->ds_pool, view->ds);
		vkDestroyImageView(dev, view->image_view, NULL);
		free(view);
	}

	vkDestroyImage(dev, texture->image, NULL);
	for (uint32_t i = 0; i < texture->mem_count; ++i) {
		vkFreeMemory(dev, texture->memories[i], NULL);
	}
	free(texture);
}

static void texture_unref(struct nx_texture *nx_texture) {
	struct nx_vk_texture *texture = vulkan_get_texture(nx_texture);
	// Kept while the buffer lives.
	if (texture->buffer != NULL) {
		nx_buffer_unlock(texture->buffer);
		return;
	}
	vulkan_texture_destroy(texture);
}

static bool texture_read_pixels(struct nx_texture *nx_texture,
		const struct nx_texture_read_pixels_options *options) {
	struct nx_vk_texture *texture = vulkan_get_texture(nx_texture);

	struct nx_box src;
	nx_texture_read_pixels_options_get_src_box(options, nx_texture, &src);

	return vulkan_read_pixels(texture->renderer, &(struct nx_vk_pixel_read){
		.src_format = texture->format->vk,
		.src_image = texture->image,
		.drm_format = options->format,
		.stride = options->stride,
		.width = src.width,
		.height = src.height,
		.src_x = src.x,
		.src_y = src.y,
		.data = nx_texture_read_pixel_options_get_data(options),
	});
}

static uint32_t texture_preferred_read_format(struct nx_texture *nx_texture) {
	struct nx_vk_texture *texture = vulkan_get_texture(nx_texture);
	return texture->format->drm;
}

static const struct nx_texture_impl texture_impl = {
	.update_from_buffer = vulkan_texture_update_from_buffer,
	.read_pixels = texture_read_pixels,
	.preferred_read_format = texture_preferred_read_format,
	.destroy = texture_unref,
};

struct nx_vk_texture *vulkan_texture_create(struct nx_vk_renderer *renderer,
		uint32_t width, uint32_t height) {
	struct nx_vk_texture *texture = calloc(1, sizeof(*texture));
	if (texture == NULL) {
		nx_log_errno(NX_ERROR, "Allocation failed");
		return NULL;
	}
	nx_texture_init(&texture->nx_texture, &renderer->nx_renderer,
		&texture_impl, width, height);
	texture->renderer = renderer;
	wl_list_insert(&renderer->textures, &texture->link);
	wl_list_init(&texture->views);
	return texture;
}

void vulkan_texture_set_format(struct nx_vk_texture *texture,
		const struct nx_vk_format *format, bool has_mutable_srgb) {
	assert(!(vulkan_format_is_ycbcr(format) && has_mutable_srgb));

	texture->format = format;
	texture->using_mutable_srgb = has_mutable_srgb;

	// Multi-planar formats lack info.
	if (drm_get_pixel_format_info(format->drm) == NULL) {
		assert(vulkan_format_is_ycbcr(format));
		return;
	}
	texture->has_alpha = pixel_format_has_alpha(format->drm);
}

struct nx_texture *vulkan_texture_from_buffer(struct nx_renderer *nx_renderer,
		struct nx_buffer *buffer) {
	struct nx_vk_renderer *renderer = vulkan_get_renderer(nx_renderer);

	struct nx_dmabuf_attributes dmabuf;
	if (nx_buffer_get_dmabuf(buffer, &dmabuf)) {
		return vulkan_texture_from_dmabuf_buffer(renderer, buffer, &dmabuf);
	}
	return vulkan_texture_from_shm_buffer(renderer, buffer);
}
