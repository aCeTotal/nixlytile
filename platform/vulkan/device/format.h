#ifndef NX_VULKAN_DEVICE_FORMAT_H
#define NX_VULKAN_DEVICE_FORMAT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan.h>
#include "platform/render/dmabuf.h"
#include "platform/vulkan/device/device.h"

struct nx_vk_format {
	uint32_t drm;
	VkFormat vk;
	VkFormat vk_srgb; // zero without an sRGB variant
};

struct nx_vk_format_modifier_props {
	VkDrmFormatModifierPropertiesEXT props;
	VkExtent2D max_extent;
	bool has_mutable_srgb;
};

struct nx_vk_modifier_list {
	uint32_t len;
	struct nx_vk_format_modifier_props *mods;
};

struct nx_vk_format_props {
	struct nx_vk_format format;

	struct {
		VkExtent2D max_extent;
		bool has_mutable_srgb;
	} shm;

	struct nx_vk_modifier_list dmabuf[NX_VK_MODIFIER_USAGE_COUNT];
};

extern const VkImageUsageFlags vulkan_shm_tex_usage;

// All mappings, supported or not.
const struct nx_vk_format *vulkan_get_format_list(size_t *len);
const struct nx_vk_format *vulkan_get_format_from_drm(uint32_t drm_format);
bool vulkan_format_is_ycbcr(const struct nx_vk_format *format);

VkImageUsageFlags vulkan_dmabuf_image_usage(enum nx_vk_modifier_usage usage);
void vulkan_log_unsupported_format(uint32_t drm_format);

void vulkan_format_props_query(struct nx_vk_device *dev, const struct nx_vk_format *format);
bool vulkan_format_props_query_modifiers(struct nx_vk_device *dev,
	struct nx_vk_format_props *props, uint32_t modifier_count);
void vulkan_format_props_finish(struct nx_vk_format_props *props);
struct nx_vk_format_props *vulkan_format_props_from_drm(struct nx_vk_device *dev,
	uint32_t drm_format);
// NULL when import is impossible.
const struct nx_vk_format_modifier_props *vulkan_format_props_import_modifier(
	const struct nx_vk_format_props *props, const struct nx_dmabuf_attributes *attribs,
	enum nx_vk_modifier_usage usage);

#endif
