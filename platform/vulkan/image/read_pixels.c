#include <string.h>
#include <vulkan/vulkan.h>
#include "platform/log/log.h"
#include "platform/render/pixel_format.h"
#include "platform/vulkan/device/format.h"
#include "platform/vulkan/image/read_pixels.h"
#include "platform/vulkan/queue/command_buffer.h"
#include "platform/vulkan/queue/stage.h"
#include "platform/vulkan/renderer.h"

static const struct nx_vk_image_use src_general = {
	.layout = VK_IMAGE_LAYOUT_GENERAL,
	.stage = VK_PIPELINE_STAGE_TRANSFER_BIT,
	.access = VK_ACCESS_MEMORY_READ_BIT,
};

static const struct nx_vk_image_use src_transfer = {
	.layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	.stage = VK_PIPELINE_STAGE_TRANSFER_BIT,
	.access = VK_ACCESS_TRANSFER_READ_BIT,
};

static const struct nx_vk_image_use dst_undefined = {
	.layout = VK_IMAGE_LAYOUT_UNDEFINED,
	.stage = VK_PIPELINE_STAGE_TRANSFER_BIT,
	.access = 0,
};

static const struct nx_vk_image_use dst_transfer = {
	.layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	.stage = VK_PIPELINE_STAGE_TRANSFER_BIT,
	.access = VK_ACCESS_TRANSFER_WRITE_BIT,
};

static const struct nx_vk_image_use dst_general = {
	.layout = VK_IMAGE_LAYOUT_GENERAL,
	.stage = VK_PIPELINE_STAGE_TRANSFER_BIT,
	.access = 0,
};

// Memoized per source format.
static bool blit_supported(struct nx_vk_renderer *renderer,
		const struct nx_vk_pixel_read *read, VkFormat dst_format) {
	struct nx_vk_readback_cache *cache = &renderer->readback;
	bool cached = cache->initialized && cache->drm_format == read->drm_format;
	if (cached && cache->src_format == read->src_format) {
		return cache->blit_supported;
	}

	VkFormatProperties dst_props = {0}, src_props = {0};
	vkGetPhysicalDeviceFormatProperties(renderer->dev->phdev, dst_format, &dst_props);
	vkGetPhysicalDeviceFormatProperties(renderer->dev->phdev, read->src_format, &src_props);
	bool supported = src_props.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT &&
		dst_props.linearTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT;
	if (cached) {
		cache->src_format = read->src_format;
		cache->blit_supported = supported;
	}
	return supported;
}

static void record_blit(VkCommandBuffer cb, const struct nx_vk_pixel_read *read,
		VkImage dst) {
	VkImageBlit region = {
		.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
		.srcSubresource.layerCount = 1,
		.srcOffsets[0] = { .x = read->src_x, .y = read->src_y, .z = 0 },
		.srcOffsets[1] = {
			.x = read->src_x + read->width,
			.y = read->src_y + read->height,
			.z = 1,
		},
		.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
		.dstSubresource.layerCount = 1,
		.dstOffsets[1] = { .x = read->width, .y = read->height, .z = 1 },
	};
	vkCmdBlitImage(cb, read->src_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_NEAREST);
}

static void record_copy(VkCommandBuffer cb, const struct nx_vk_pixel_read *read,
		VkImage dst) {
	nx_log(NX_DEBUG, "vulkan_read_pixels: blit unsupported, falling back to vkCmdCopyImage.");
	VkImageCopy region = {
		.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
		.srcSubresource.layerCount = 1,
		.srcOffset = { .x = read->src_x, .y = read->src_y },
		.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
		.dstSubresource.layerCount = 1,
		.extent = { .width = read->width, .height = read->height, .depth = 1 },
	};
	vkCmdCopyImage(cb, read->src_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
}

static bool copy_rows(struct nx_vk_renderer *renderer, const struct nx_vk_pixel_read *read,
		uint32_t bytes_per_pixel) {
	VkDevice dev = renderer->dev->dev;
	const struct nx_vk_readback_cache *cache = &renderer->readback;

	VkImageSubresource sub = {
		.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
		.arrayLayer = 0,
		.mipLevel = 0,
	};
	VkSubresourceLayout layout;
	vkGetImageSubresourceLayout(dev, cache->image, &sub, &layout);

	VkMappedMemoryRange mem_range = {
		.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
		.memory = cache->memory,
		.offset = 0,
		.size = VK_WHOLE_SIZE,
	};
	VkResult res = vkInvalidateMappedMemoryRanges(dev, 1, &mem_range);
	if (res != VK_SUCCESS) {
		nx_vk_error("vkInvalidateMappedMemoryRanges", res);
		return false;
	}

	const char *src = (const char *)cache->map + layout.offset;
	unsigned char *dst = read->data;
	uint32_t pack_stride = layout.rowPitch;
	if (pack_stride == read->stride) {
		memcpy(dst, src, read->height * read->stride);
		return true;
	}
	for (size_t i = 0; i < read->height; ++i) {
		memcpy(dst + i * read->stride, src + i * pack_stride, read->width * bytes_per_pixel);
	}
	return true;
}

static bool check_format(const struct nx_pixel_format_info *info, uint32_t drm_format) {
	if (!info) {
		nx_log(NX_ERROR, "vulkan_read_pixels: could not find pixel format info "
			"for DRM format 0x%08x", drm_format);
		return false;
	}
	if (pixel_format_info_pixels_per_block(info) != 1) {
		nx_log(NX_ERROR, "vulkan_read_pixels: block formats are not supported");
		return false;
	}
	return true;
}

bool vulkan_read_pixels(struct nx_vk_renderer *renderer, const struct nx_vk_pixel_read *read) {
	const struct nx_pixel_format_info *info = drm_get_pixel_format_info(read->drm_format);
	if (!check_format(info, read->drm_format)) {
		return false;
	}

	const struct nx_vk_format *format = vulkan_get_format_from_drm(read->drm_format);
	if (!format) {
		nx_log(NX_ERROR, "vulkan_read_pixels: no vulkan format "
			"matching drm format 0x%08x available", read->drm_format);
		return false;
	}

	bool blit = blit_supported(renderer, read, format->vk);
	if (!blit && read->src_format != format->vk) {
		nx_log(NX_ERROR, "vulkan_read_pixels: blit unsupported and no manual "
			"conversion available from src to dst format.");
		return false;
	}
	if (!vulkan_readback_prepare(renderer, read, format->vk)) {
		return false;
	}
	renderer->readback.src_format = read->src_format;
	renderer->readback.blit_supported = blit;

	VkCommandBuffer cb = vulkan_record_stage_cb(renderer);
	if (cb == VK_NULL_HANDLE) {
		return false;
	}

	VkImage dst = renderer->readback.image;
	vulkan_change_layout(cb, dst, dst_undefined, dst_transfer);
	vulkan_change_layout(cb, read->src_image, src_general, src_transfer);
	if (blit) {
		record_blit(cb, read, dst);
	} else {
		record_copy(cb, read, dst);
	}
	vulkan_change_layout(cb, dst, dst_transfer, dst_general);
	vulkan_change_layout(cb, read->src_image, src_transfer, src_general);

	// Image and mapping stay cached.
	return vulkan_submit_stage_wait(renderer) &&
		copy_rows(renderer, read, info->bytes_per_block);
}
