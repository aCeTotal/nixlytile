#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <xf86drm.h>
#include <vulkan/vulkan.h>
#include "platform/log/log.h"
#include "platform/vulkan/device/format.h"

#define STATUS_LEN 256

static const char missing_features[] = "missing required features";

static const VkFormatFeatureFlags render_features =
	VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
	VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT;
// Linear filter wanted, not required.
static const VkFormatFeatureFlags dma_tex_features =
	VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
	VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
static const VkFormatFeatureFlags ycbcr_tex_features =
	VK_FORMAT_FEATURE_SAMPLED_IMAGE_YCBCR_CONVERSION_LINEAR_FILTER_BIT |
	VK_FORMAT_FEATURE_MIDPOINT_CHROMA_SAMPLES_BIT;

struct modifier_query {
	struct nx_vk_device *dev;
	struct nx_vk_format_props *props;
	const VkDrmFormatModifierPropertiesEXT *mod;
};

// NULL if importable, else why.
static const char *probe_modifier(const struct modifier_query *q, VkFormat variant,
		VkImageUsageFlags usage, struct nx_vk_format_modifier_props *out) {
	VkFormat vk_format = q->props->format.vk;
	VkFormat view_formats[2] = { vk_format, variant };
	VkImageFormatListCreateInfoKHR listi = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO_KHR,
		.pViewFormats = view_formats,
		.viewFormatCount = variant ? 2 : 1,
	};
	VkPhysicalDeviceImageDrmFormatModifierInfoEXT modi = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT,
		.pNext = &listi,
		.drmFormatModifier = q->mod->drmFormatModifier,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	};
	VkPhysicalDeviceExternalImageFormatInfo efmti = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
		.pNext = &modi,
		.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
	};
	VkPhysicalDeviceImageFormatInfo2 fmti = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
		.pNext = &efmti,
		.type = VK_IMAGE_TYPE_2D,
		.format = vk_format,
		.usage = usage,
		.flags = variant ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0,
		.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
	};

	VkExternalImageFormatProperties efmtp = {
		.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES,
	};
	VkImageFormatProperties2 ifmtp = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
		.pNext = &efmtp,
	};

	VkResult res = vkGetPhysicalDeviceImageFormatProperties2(q->dev->phdev, &fmti, &ifmtp);
	if (res == VK_ERROR_FORMAT_NOT_SUPPORTED) {
		return "unsupported format";
	}
	if (res != VK_SUCCESS) {
		nx_vk_error("vkGetPhysicalDeviceImageFormatProperties2", res);
		return "failed to get format properties";
	}
	if (!(efmtp.externalMemoryProperties.externalMemoryFeatures &
			VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT)) {
		return "import not supported";
	}

	VkExtent3D me = ifmtp.imageFormatProperties.maxExtent;
	*out = (struct nx_vk_format_modifier_props){
		.props = *q->mod,
		.max_extent.width = me.width,
		.max_extent.height = me.height,
	};
	return NULL;
}

// Tries sRGB-capable first.
static const char *probe_modifier_usage(const struct modifier_query *q,
		VkImageUsageFlags usage, struct nx_vk_format_modifier_props *out) {
	VkFormat vk_srgb = q->props->format.vk_srgb;
	const char *errmsg = probe_modifier(q, vk_srgb, usage, out);
	if (!errmsg) {
		out->has_mutable_srgb = vk_srgb != 0;
		return NULL;
	}
	if (!vk_srgb) {
		return errmsg;
	}
	return probe_modifier(q, 0, usage, out);
}

// Returns NULL once recorded.
static const char *add_modifier(const struct modifier_query *q,
		enum nx_vk_modifier_usage usage) {
	struct nx_vk_format_modifier_props p = {0};
	const char *errmsg = probe_modifier_usage(q, vulkan_dmabuf_image_usage(usage), &p);
	if (errmsg) {
		return errmsg;
	}

	struct nx_vk_modifier_list *list = &q->props->dmabuf[usage];
	list->mods[list->len++] = p;
	nx_drm_format_set_add(&q->dev->dmabuf_formats[usage], q->props->format.drm,
		q->mod->drmFormatModifier);
	return NULL;
}

static void format_status(char status[static STATUS_LEN], const char *usage,
		const char *errmsg) {
	if (errmsg) {
		snprintf(status, STATUS_LEN, "✗ %s (%s)", usage, errmsg);
		return;
	}
	snprintf(status, STATUS_LEN, "✓ %s", usage);
}

static void log_modifier(const VkDrmFormatModifierPropertiesEXT *m,
		const char *texture_err, const char *render_err) {
	char texture_status[STATUS_LEN], render_status[STATUS_LEN];
	format_status(texture_status, "texture", texture_err);
	format_status(render_status, "render", render_err);

	char *modifier_name = drmGetFormatModifierName(m->drmFormatModifier);
	nx_log(NX_DEBUG, "    DMA-BUF modifier %s "
		"(0x%016"PRIX64", %"PRIu32" planes): %s  %s",
		modifier_name ? modifier_name : "<unknown>", m->drmFormatModifier,
		m->drmFormatModifierPlaneCount, texture_status, render_status);
	free(modifier_name);
}

static bool has_features(const VkDrmFormatModifierPropertiesEXT *m,
		VkFormatFeatureFlags features) {
	return (m->drmFormatModifierTilingFeatures & features) == features;
}

static bool query_modifier(struct nx_vk_device *dev, struct nx_vk_format_props *props,
		const VkDrmFormatModifierPropertiesEXT *m) {
	struct modifier_query q = { .dev = dev, .props = props, .mod = m };
	bool ycbcr = vulkan_format_is_ycbcr(&props->format);
	VkFormatFeatureFlags tex_features = dma_tex_features;
	if (ycbcr) {
		tex_features |= ycbcr_tex_features;
	}

	const char *render_err = missing_features;
	if (has_features(m, render_features) && !ycbcr) {
		render_err = add_modifier(&q, NX_VK_MODIFIER_RENDER);
	}
	const char *texture_err = missing_features;
	if (has_features(m, tex_features)) {
		texture_err = add_modifier(&q, NX_VK_MODIFIER_TEXTURE);
	}

	log_modifier(m, texture_err, render_err);
	return !render_err || !texture_err;
}

static bool query_modifier_list(struct nx_vk_device *dev, struct nx_vk_format_props *props,
		const VkDrmFormatModifierPropertiesListEXT *modp) {
	bool found = false;
	for (uint32_t i = 0; i < modp->drmFormatModifierCount; ++i) {
		found |= query_modifier(dev, props, &modp->pDrmFormatModifierProperties[i]);
	}
	return found;
}

static bool alloc_modifier_lists(struct nx_vk_format_props *props, uint32_t count) {
	bool ok = true;
	for (size_t i = 0; i < NX_VK_MODIFIER_USAGE_COUNT; i++) {
		props->dmabuf[i].mods = calloc(count, sizeof(*props->dmabuf[i].mods));
		ok = ok && props->dmabuf[i].mods;
	}
	if (ok) {
		return true;
	}

	nx_log_errno(NX_ERROR, "Allocation failed");
	for (size_t i = 0; i < NX_VK_MODIFIER_USAGE_COUNT; i++) {
		free(props->dmabuf[i].mods);
		props->dmabuf[i].mods = NULL;
	}
	return false;
}

bool vulkan_format_props_query_modifiers(struct nx_vk_device *dev,
		struct nx_vk_format_props *props, uint32_t modifier_count) {
	VkDrmFormatModifierPropertiesListEXT modp = {
		.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT,
		.drmFormatModifierCount = modifier_count,
	};
	VkFormatProperties2 fmtp = {
		.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
		.pNext = &modp,
	};

	modp.pDrmFormatModifierProperties =
		calloc(modifier_count, sizeof(*modp.pDrmFormatModifierProperties));
	if (!modp.pDrmFormatModifierProperties) {
		nx_log_errno(NX_ERROR, "Allocation failed");
		return false;
	}

	vkGetPhysicalDeviceFormatProperties2(dev->phdev, props->format.vk, &fmtp);

	bool found = alloc_modifier_lists(props, modp.drmFormatModifierCount) &&
		query_modifier_list(dev, props, &modp);

	free(modp.pDrmFormatModifierProperties);
	return found;
}
