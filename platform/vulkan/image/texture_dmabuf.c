#include <string.h>
#include "platform/buffer/buffer.h"
#include "platform/container/addon.h"
#include "platform/vulkan/image/texture.h"
#include "platform/vulkan/renderer.h"

static struct nx_vk_texture *texture_from_dmabuf(struct nx_vk_renderer *renderer,
		const struct nx_dmabuf_attributes *attribs) {
	const struct nx_vk_format_props *fmt =
		vulkan_format_props_from_drm(renderer->dev, attribs->format);
	if (fmt == NULL) {
		vulkan_log_unsupported_format(attribs->format);
		return NULL;
	}

	struct nx_vk_texture *texture = vulkan_texture_create(renderer,
		attribs->width, attribs->height);
	if (texture == NULL) {
		return NULL;
	}

	struct nx_vk_dmabuf_image imported;
	if (!vulkan_import_dmabuf(renderer, attribs, NX_VK_MODIFIER_TEXTURE, &imported)) {
		vulkan_texture_destroy(texture);
		return NULL;
	}
	texture->image = imported.image;
	texture->mem_count = imported.mem_count;
	memcpy(texture->memories, imported.memories, sizeof(texture->memories));
	vulkan_texture_set_format(texture, &fmt->format, imported.has_mutable_srgb);
	texture->dmabuf_imported = true;
	return texture;
}

// May defer until GPU completion.
static void handle_buffer_destroy(struct nx_addon *addon) {
	struct nx_vk_texture *texture = wl_container_of(addon, texture, buffer_addon);
	vulkan_texture_destroy(texture);
}

static const struct nx_addon_interface buffer_addon_impl = {
	.name = "nx_vk_texture",
	.destroy = handle_buffer_destroy,
};

// One texture per buffer.
struct nx_texture *vulkan_texture_from_dmabuf_buffer(struct nx_vk_renderer *renderer,
		struct nx_buffer *buffer, struct nx_dmabuf_attributes *dmabuf) {
	struct nx_addon *addon = nx_addon_find(&buffer->addons, renderer, &buffer_addon_impl);
	if (addon != NULL) {
		struct nx_vk_texture *texture = wl_container_of(addon, texture, buffer_addon);
		nx_buffer_lock(texture->buffer);
		return &texture->nx_texture;
	}

	struct nx_vk_texture *texture = texture_from_dmabuf(renderer, dmabuf);
	if (texture == NULL) {
		return NULL;
	}
	texture->buffer = nx_buffer_lock(buffer);
	nx_addon_init(&texture->buffer_addon, &buffer->addons, renderer, &buffer_addon_impl);
	return &texture->nx_texture;
}
