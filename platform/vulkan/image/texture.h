#ifndef NX_VULKAN_IMAGE_TEXTURE_H
#define NX_VULKAN_IMAGE_TEXTURE_H

#include <stdbool.h>
#include <stdint.h>
#include <pixman.h>
#include <vulkan/vulkan.h>
#include <wayland-util.h>
#include "platform/container/addon.h"
#include "platform/render/dmabuf.h"
#include "platform/render/texture.h"
#include "platform/vulkan/device/format.h"

struct nx_vk_renderer;
struct nx_vk_command_buffer;
struct nx_vk_pipeline_layout;
struct nx_vk_descriptor_pool;

struct nx_vk_texture {
	struct nx_texture nx_texture;
	struct nx_vk_renderer *renderer;
	uint32_t mem_count;
	VkDeviceMemory memories[NX_DMABUF_MAX_PLANES];
	VkImage image;
	const struct nx_vk_format *format;
	struct nx_vk_command_buffer *last_used_cb; // defers destruction
	bool dmabuf_imported;
	bool owned; // held by our queue
	bool pass_synced; // queued in this pass
	bool has_alpha;
	bool using_mutable_srgb; // readable through an _SRGB view
	struct wl_list foreign_link; // nx_vk_renderer.foreign_textures
	struct wl_list destroy_link; // nx_vk_command_buffer.destroy_textures
	struct wl_list link; // nx_vk_renderer.textures

	// Source buffer, if any.
	struct nx_buffer *buffer;
	struct nx_addon buffer_addon;

	struct wl_list views; // nx_vk_texture_view.link
};

struct nx_vk_texture_view {
	struct wl_list link; // nx_vk_texture.views
	const struct nx_vk_pipeline_layout *layout;
	bool srgb;

	VkDescriptorSet ds;
	VkImageView image_view;
	struct nx_vk_descriptor_pool *ds_pool;
};

// An imported DMA-BUF image.
struct nx_vk_dmabuf_image {
	VkImage image;
	VkDeviceMemory memories[NX_DMABUF_MAX_PLANES];
	uint32_t mem_count;
	bool has_mutable_srgb;
};

struct nx_vk_texture *vulkan_get_texture(struct nx_texture *nx_texture);
struct nx_vk_texture *vulkan_texture_create(struct nx_vk_renderer *renderer,
	uint32_t width, uint32_t height);
void vulkan_texture_destroy(struct nx_vk_texture *texture);
void vulkan_texture_set_format(struct nx_vk_texture *texture,
	const struct nx_vk_format *format, bool has_mutable_srgb);
struct nx_texture *vulkan_texture_from_buffer(struct nx_renderer *nx_renderer,
	struct nx_buffer *buffer);

struct nx_texture *vulkan_texture_from_shm_buffer(struct nx_vk_renderer *renderer,
	struct nx_buffer *buffer);
bool vulkan_texture_update_from_buffer(struct nx_texture *nx_texture,
	struct nx_buffer *buffer, const pixman_region32_t *damage);

struct nx_texture *vulkan_texture_from_dmabuf_buffer(struct nx_vk_renderer *renderer,
	struct nx_buffer *buffer, struct nx_dmabuf_attributes *dmabuf);
bool vulkan_import_dmabuf(struct nx_vk_renderer *renderer,
	const struct nx_dmabuf_attributes *attribs, enum nx_vk_modifier_usage usage,
	struct nx_vk_dmabuf_image *out);

struct nx_vk_texture_view *vulkan_texture_get_or_create_view(struct nx_vk_texture *texture,
	const struct nx_vk_pipeline_layout *layout, bool srgb);

#endif
