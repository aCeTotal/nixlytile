#include <assert.h>
#include <drm_fourcc.h>
#include <inttypes.h>
#include <stdlib.h>
#include <xf86drm.h>
#include <vulkan/vulkan.h>
#include "platform/log/log.h"
#include "platform/render/pixel_format.h"
#include "platform/vulkan/device/format.h"

const VkImageUsageFlags vulkan_shm_tex_usage =
	VK_IMAGE_USAGE_SAMPLED_BIT |
	VK_IMAGE_USAGE_TRANSFER_DST_BIT |
	VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

// Linear filter wanted, not required.
static const VkFormatFeatureFlags shm_tex_features =
	VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
	VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
	VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
	VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;

static const VkImageUsageFlags dmabuf_image_usage[NX_VK_MODIFIER_USAGE_COUNT] = {
	[NX_VK_MODIFIER_RENDER] = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
	[NX_VK_MODIFIER_TEXTURE] = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
};

VkImageUsageFlags vulkan_dmabuf_image_usage(enum nx_vk_modifier_usage usage) {
	return dmabuf_image_usage[usage];
}

void vulkan_log_unsupported_format(uint32_t drm_format) {
	char *format_name = drmGetFormatName(drm_format);
	nx_log(NX_ERROR, "Unsupported pixel format %s (0x%08"PRIX32")",
		format_name, drm_format);
	free(format_name);
}

// Sets max extent; NULL if supported.
static const char *probe_shm_format(VkPhysicalDevice phdev, VkFormat variant,
		struct nx_vk_format_props *props) {
	VkFormat vk_format = props->format.vk;
	VkFormat view_formats[2] = { vk_format, variant };
	VkImageFormatListCreateInfoKHR listi = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO_KHR,
		.pViewFormats = view_formats,
		.viewFormatCount = variant ? 2 : 1,
	};
	VkPhysicalDeviceImageFormatInfo2 fmti = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
		.pNext = &listi,
		.type = VK_IMAGE_TYPE_2D,
		.format = vk_format,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = vulkan_shm_tex_usage,
		.flags = variant ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0,
	};
	VkImageFormatProperties2 ifmtp = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
	};

	VkResult res = vkGetPhysicalDeviceImageFormatProperties2(phdev, &fmti, &ifmtp);
	if (res == VK_ERROR_FORMAT_NOT_SUPPORTED) {
		return "unsupported format";
	}
	if (res != VK_SUCCESS) {
		nx_vk_error("vkGetPhysicalDeviceImageFormatProperties2", res);
		return "failed to get format properties";
	}

	VkExtent3D extent = ifmtp.imageFormatProperties.maxExtent;
	props->shm.max_extent.width = extent.width;
	props->shm.max_extent.height = extent.height;
	return NULL;
}

// Fills props->shm; NULL when supported.
static const char *query_shm(struct nx_vk_device *dev, VkFormatFeatureFlags features,
		struct nx_vk_format_props *props) {
	const struct nx_vk_format *format = &props->format;
	if ((features & shm_tex_features) != shm_tex_features ||
			vulkan_format_is_ycbcr(format) || !drm_get_pixel_format_info(format->drm)) {
		return "missing required features";
	}

	bool has_mutable_srgb = format->vk_srgb != 0;
	const char *errmsg = probe_shm_format(dev->phdev, format->vk_srgb, props);
	if (errmsg && format->vk_srgb) {
		has_mutable_srgb = false;
		errmsg = probe_shm_format(dev->phdev, 0, props);
	}
	if (errmsg) {
		return errmsg;
	}

	props->shm.has_mutable_srgb = has_mutable_srgb;
	nx_drm_format_set_add(&dev->shm_texture_formats, format->drm, DRM_FORMAT_MOD_LINEAR);
	return NULL;
}

void vulkan_format_props_query(struct nx_vk_device *dev, const struct nx_vk_format *format) {
	if (vulkan_format_is_ycbcr(format) && !dev->sampler_ycbcr_conversion) {
		return;
	}

	char *format_name = drmGetFormatName(format->drm);
	nx_log(NX_DEBUG, "  %s (0x%08"PRIX32")",
		format_name ? format_name : "<unknown>", format->drm);
	free(format_name);

	VkDrmFormatModifierPropertiesListEXT modp = {
		.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT,
	};
	VkFormatProperties2 fmtp = {
		.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
		.pNext = &modp,
	};
	vkGetPhysicalDeviceFormatProperties2(dev->phdev, format->vk, &fmtp);

	struct nx_vk_format_props props = { .format = *format };
	const char *errmsg = query_shm(dev, fmtp.formatProperties.optimalTilingFeatures, &props);
	if (errmsg) {
		nx_log(NX_DEBUG, "    Shared memory: ✗ texture (%s)", errmsg);
	} else {
		nx_log(NX_DEBUG, "    Shared memory: ✓ texture");
	}

	bool supported = errmsg == NULL;
	if (modp.drmFormatModifierCount > 0) {
		supported |= vulkan_format_props_query_modifiers(dev, &props,
			modp.drmFormatModifierCount);
	}

	if (!supported) {
		vulkan_format_props_finish(&props);
		return;
	}
	dev->format_props[dev->format_prop_count++] = props;
}

void vulkan_format_props_finish(struct nx_vk_format_props *props) {
	for (size_t i = 0; i < NX_VK_MODIFIER_USAGE_COUNT; i++) {
		free(props->dmabuf[i].mods);
	}
}

struct nx_vk_format_props *vulkan_format_props_from_drm(struct nx_vk_device *dev,
		uint32_t drm_format) {
	for (uint32_t i = 0; i < dev->format_prop_count; ++i) {
		if (dev->format_props[i].format.drm == drm_format) {
			return &dev->format_props[i];
		}
	}
	return NULL;
}

static const struct nx_vk_format_modifier_props *find_modifier(
		const struct nx_vk_format_props *props, uint64_t mod,
		enum nx_vk_modifier_usage usage) {
	const struct nx_vk_modifier_list *list = &props->dmabuf[usage];
	for (uint32_t i = 0; i < list->len; ++i) {
		if (list->mods[i].props.drmFormatModifier == mod) {
			return &list->mods[i];
		}
	}
	return NULL;
}

const struct nx_vk_format_modifier_props *vulkan_format_props_import_modifier(
		const struct nx_vk_format_props *fmt, const struct nx_dmabuf_attributes *attribs,
		enum nx_vk_modifier_usage usage) {
	uint32_t plane_count = attribs->n_planes;
	assert(plane_count < NX_DMABUF_MAX_PLANES);

	const struct nx_vk_format_modifier_props *mod =
		find_modifier(fmt, attribs->modifier, usage);
	if (!mod) {
		char *format_name = drmGetFormatName(attribs->format);
		char *modifier_name = drmGetFormatModifierName(attribs->modifier);
		nx_log(NX_ERROR, "Format %s (0x%08"PRIX32") can't be used with modifier "
			"%s (0x%016"PRIX64")", format_name, attribs->format,
			modifier_name, attribs->modifier);
		free(format_name);
		free(modifier_name);
		return NULL;
	}

	if ((uint32_t)attribs->width > mod->max_extent.width ||
			(uint32_t)attribs->height > mod->max_extent.height) {
		nx_log(NX_ERROR, "DMA-BUF is too large to import "
			"(%"PRIi32"x%"PRIi32" > %"PRIu32"x%"PRIu32")", attribs->width,
			attribs->height, mod->max_extent.width, mod->max_extent.height);
		return NULL;
	}

	if (mod->props.drmFormatModifierPlaneCount != plane_count) {
		nx_log(NX_ERROR, "Number of planes (%d) does not match format (%d)",
			plane_count, mod->props.drmFormatModifierPlaneCount);
		return NULL;
	}
	return mod;
}
