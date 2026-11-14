#include <assert.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include "platform/buffer/buffer.h"
#include "platform/log/log.h"
#include "platform/render/pixel_format.h"
#include "platform/vulkan/image/texture.h"
#include "platform/vulkan/queue/command_buffer.h"
#include "platform/vulkan/queue/stage.h"
#include "platform/vulkan/renderer.h"

struct pixel_upload {
	uint32_t drm_format;
	uint32_t stride;
	const pixman_region32_t *region;
	const void *data;
};

static const struct nx_vk_image_use transfer_dst = {
	.layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	.stage = VK_PIPELINE_STAGE_TRANSFER_BIT,
	.access = VK_ACCESS_TRANSFER_WRITE_BIT,
};

static const struct nx_vk_image_use sampled = {
	.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	.stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
	.access = VK_ACCESS_SHADER_READ_BIT,
};

static const struct nx_vk_image_use undefined = {
	.layout = VK_IMAGE_LAYOUT_UNDEFINED,
	.stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
	.access = 0,
};

static uint32_t staging_size(const struct nx_vk_texture *texture,
		const struct nx_pixel_format_info *format_info, const pixman_region32_t *region) {
	int rects_len = 0;
	const pixman_box32_t *rects = pixman_region32_rectangles(region, &rects_len);
	uint32_t size = 0;
	for (int i = 0; i < rects_len; i++) {
		assert((uint32_t)rects[i].x2 <= texture->nx_texture.width);
		assert((uint32_t)rects[i].y2 <= texture->nx_texture.height);
		uint32_t width = rects[i].x2 - rects[i].x1;
		uint32_t height = rects[i].y2 - rects[i].y1;
		size += height * pixel_format_info_min_stride(format_info, width);
	}
	return size;
}

struct rect_packer {
	const struct pixel_upload *upload;
	const struct nx_pixel_format_info *format_info;
	uint32_t texture_width;
};

// Returns the bytes written.
static uint32_t pack_rect(const struct rect_packer *packer, const pixman_box32_t *rect,
		char *map) {
	const struct pixel_upload *upload = packer->upload;
	const struct nx_pixel_format_info *format_info = packer->format_info;
	uint32_t width = rect->x2 - rect->x1;
	uint32_t height = rect->y2 - rect->y1;
	uint32_t src_x = rect->x1;
	uint32_t src_y = rect->y1;
	uint32_t packed_stride = (uint32_t)pixel_format_info_min_stride(format_info, width);

	const char *src = upload->data;
	src += upload->stride * src_y;
	src += format_info->bytes_per_block * src_x;
	if (src_x == 0 && width == packer->texture_width && upload->stride == packed_stride) {
		memcpy(map, src, packed_stride * height);
		return packed_stride * height;
	}

	for (uint32_t row = 0; row < height; ++row) {
		memcpy(map, src, packed_stride);
		src += upload->stride;
		map += packed_stride;
	}
	return packed_stride * height;
}

static VkBufferImageCopy region_copy(const pixman_box32_t *rect, VkDeviceSize offset) {
	return (VkBufferImageCopy){
		.imageExtent.width = rect->x2 - rect->x1,
		.imageExtent.height = rect->y2 - rect->y1,
		.imageExtent.depth = 1,
		.imageOffset.x = rect->x1,
		.imageOffset.y = rect->y1,
		.imageOffset.z = 0,
		.bufferOffset = offset,
		.bufferRowLength = rect->x2 - rect->x1,
		.bufferImageHeight = rect->y2 - rect->y1,
		.imageSubresource.mipLevel = 0,
		.imageSubresource.baseArrayLayer = 0,
		.imageSubresource.layerCount = 1,
		.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
	};
}

// Staged copy, ends shader-readable.
static bool write_pixels(struct nx_vk_texture *texture, const struct pixel_upload *upload,
		const struct nx_vk_image_use *from) {
	struct nx_vk_renderer *renderer = texture->renderer;
	const struct nx_pixel_format_info *format_info =
		drm_get_pixel_format_info(texture->format->drm);
	assert(format_info);

	int rects_len = 0;
	const pixman_box32_t *rects = pixman_region32_rectangles(upload->region, &rects_len);
	uint32_t bsize = staging_size(texture, format_info, upload->region);

	VkBufferImageCopy *copies = calloc((size_t)rects_len, sizeof(*copies));
	if (!copies) {
		nx_log(NX_ERROR, "Failed to allocate image copy parameters");
		return false;
	}

	struct nx_vk_buffer_span span = vulkan_get_stage_span(renderer, bsize,
		format_info->bytes_per_block);
	if (!span.buffer || span.size != bsize) {
		nx_log(NX_ERROR, "Failed to retrieve staging buffer");
		free(copies);
		return false;
	}

	struct rect_packer packer = {
		.upload = upload,
		.format_info = format_info,
		.texture_width = texture->nx_texture.width,
	};
	char *map = vulkan_span_map(&span);
	VkDeviceSize offset = span.offset;
	for (int i = 0; i < rects_len; i++) {
		copies[i] = region_copy(&rects[i], offset);
		uint32_t written = pack_rect(&packer, &rects[i], map);
		map += written;
		offset += written;
	}

	VkCommandBuffer cb = vulkan_record_stage_cb(renderer);
	if (cb == VK_NULL_HANDLE) {
		free(copies);
		return false;
	}

	vulkan_change_layout(cb, texture->image, *from, transfer_dst);
	vkCmdCopyBufferToImage(cb, span.buffer->buffer, texture->image,
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, (uint32_t)rects_len, copies);
	vulkan_change_layout(cb, texture->image, transfer_dst, sampled);
	texture->last_used_cb = renderer->stage.cb;

	free(copies);
	return true;
}

bool vulkan_texture_update_from_buffer(struct nx_texture *nx_texture,
		struct nx_buffer *buffer, const pixman_region32_t *damage) {
	struct nx_vk_texture *texture = vulkan_get_texture(nx_texture);

	void *data;
	uint32_t format;
	size_t stride;
	if (!nx_buffer_begin_data_ptr_access(buffer,
			NX_BUFFER_DATA_PTR_ACCESS_READ, &data, &format, &stride)) {
		return false;
	}

	bool ok = format == texture->format->drm && write_pixels(texture,
		&(struct pixel_upload){ .stride = stride, .region = damage, .data = data },
		&sampled);

	nx_buffer_end_data_ptr_access(buffer);
	return ok;
}

static bool create_shm_image(struct nx_vk_texture *texture,
		const struct nx_vk_format_props *fmt) {
	struct nx_vk_renderer *renderer = texture->renderer;
	VkFormat view_formats[] = {
		fmt->format.vk,
		fmt->format.vk_srgb,
	};
	VkImageFormatListCreateInfoKHR list_info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO_KHR,
		.pViewFormats = view_formats,
		.viewFormatCount = sizeof(view_formats) / sizeof(view_formats[0]),
	};
	VkImageCreateInfo img_info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.pNext = fmt->shm.has_mutable_srgb ? &list_info : NULL,
		.flags = fmt->shm.has_mutable_srgb ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = texture->format->vk,
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		.extent = (VkExtent3D){ texture->nx_texture.width, texture->nx_texture.height, 1 },
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = vulkan_shm_tex_usage,
	};

	VkResult res = vkCreateImage(renderer->dev->dev, &img_info, NULL, &texture->image);
	if (res != VK_SUCCESS) {
		nx_vk_error("vkCreateImage failed", res);
		return false;
	}

	texture->memories[0] = vulkan_alloc_image_memory(renderer->dev, texture->image,
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (texture->memories[0] == VK_NULL_HANDLE) {
		return false;
	}
	texture->mem_count = 1;
	return true;
}

static struct nx_vk_texture *texture_from_pixels(struct nx_vk_renderer *renderer,
		struct nx_buffer *buffer, const struct pixel_upload *upload) {
	uint32_t width = buffer->width;
	uint32_t height = buffer->height;
	const struct nx_vk_format_props *fmt =
		vulkan_format_props_from_drm(renderer->dev, upload->drm_format);
	if (fmt == NULL || vulkan_format_is_ycbcr(&fmt->format)) {
		vulkan_log_unsupported_format(upload->drm_format);
		return NULL;
	}

	if (width > fmt->shm.max_extent.width || height > fmt->shm.max_extent.height) {
		nx_log(NX_ERROR, "Texture is too large to upload "
			"(%"PRIu32"x%"PRIu32" > %"PRIu32"x%"PRIu32")",
			width, height, fmt->shm.max_extent.width, fmt->shm.max_extent.height);
		return NULL;
	}

	struct nx_vk_texture *texture = vulkan_texture_create(renderer, width, height);
	if (texture == NULL) {
		return NULL;
	}
	vulkan_texture_set_format(texture, &fmt->format, fmt->shm.has_mutable_srgb);

	if (!create_shm_image(texture, fmt) || !write_pixels(texture, upload, &undefined)) {
		vulkan_texture_destroy(texture);
		return NULL;
	}
	return texture;
}

struct nx_texture *vulkan_texture_from_shm_buffer(struct nx_vk_renderer *renderer,
		struct nx_buffer *buffer) {
	void *data;
	uint32_t format;
	size_t stride;
	if (!nx_buffer_begin_data_ptr_access(buffer,
			NX_BUFFER_DATA_PTR_ACCESS_READ, &data, &format, &stride)) {
		return NULL;
	}

	pixman_region32_t region;
	pixman_region32_init_rect(&region, 0, 0, buffer->width, buffer->height);
	struct pixel_upload upload = {
		.drm_format = format,
		.stride = stride,
		.region = &region,
		.data = data,
	};
	struct nx_vk_texture *texture = texture_from_pixels(renderer, buffer, &upload);
	pixman_region32_fini(&region);

	nx_buffer_end_data_ptr_access(buffer);
	return texture ? &texture->nx_texture : NULL;
}
