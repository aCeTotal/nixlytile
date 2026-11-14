#include <assert.h>
#include <stdlib.h>
#include <vulkan/vulkan.h>
#include "platform/container/array.h"
#include "platform/log/log.h"
#include "platform/vulkan/queue/stage.h"
#include "platform/vulkan/renderer.h"

#define NO_SPACE ((VkDeviceSize)-1)
// Idle collections before freeing.
#define STAGE_GC_IDLE_LIMIT 1000

static const VkDeviceSize min_stage_size = 1024 * 1024;
static const VkDeviceSize max_stage_size = 256 * min_stage_size;

static void stage_buffer_destroy(struct nx_vk_renderer *r, struct nx_vk_stage_buffer *buffer) {
	VkDevice dev = r->dev->dev;
	wl_array_release(&buffer->watermarks);
	if (buffer->cpu_mapping) {
		vkUnmapMemory(dev, buffer->memory);
	}
	vkDestroyBuffer(dev, buffer->buffer, NULL);
	vkFreeMemory(dev, buffer->memory, NULL);

	wl_list_remove(&buffer->link);
	free(buffer);
}

static bool init_stage_memory(struct nx_vk_renderer *r, struct nx_vk_stage_buffer *buf) {
	VkDevice dev = r->dev->dev;
	VkMemoryRequirements mem_reqs;
	vkGetBufferMemoryRequirements(dev, buf->buffer, &mem_reqs);

	int mem_type_index = vulkan_find_mem_type(r->dev,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		mem_reqs.memoryTypeBits);
	if (mem_type_index < 0) {
		nx_log(NX_ERROR, "Failed to find memory type");
		return false;
	}

	VkMemoryAllocateInfo mem_info = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = mem_reqs.size,
		.memoryTypeIndex = (uint32_t)mem_type_index,
	};
	VkResult res = vkAllocateMemory(dev, &mem_info, NULL, &buf->memory);
	if (res != VK_SUCCESS) {
		nx_vk_error("vkAllocateMemory", res);
		return false;
	}

	res = vkBindBufferMemory(dev, buf->buffer, buf->memory, 0);
	if (res != VK_SUCCESS) {
		nx_vk_error("vkBindBufferMemory", res);
		return false;
	}

	res = vkMapMemory(dev, buf->memory, 0, VK_WHOLE_SIZE, 0, &buf->cpu_mapping);
	if (res != VK_SUCCESS) {
		nx_vk_error("vkMapMemory", res);
		return false;
	}
	return true;
}

static struct nx_vk_stage_buffer *stage_buffer_create(struct nx_vk_renderer *r,
		VkDeviceSize bsize) {
	struct nx_vk_stage_buffer *buf = calloc(1, sizeof(*buf));
	if (!buf) {
		nx_log_errno(NX_ERROR, "Allocation failed");
		return NULL;
	}
	wl_list_init(&buf->link);

	VkBufferCreateInfo buf_info = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = bsize,
		.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT |
			VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
			VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	};
	VkResult res = vkCreateBuffer(r->dev->dev, &buf_info, NULL, &buf->buffer);
	if (res != VK_SUCCESS) {
		nx_vk_error("vkCreateBuffer", res);
		stage_buffer_destroy(r, buf);
		return NULL;
	}

	if (!init_stage_memory(r, buf)) {
		stage_buffer_destroy(r, buf);
		return NULL;
	}
	buf->buf_size = bsize;
	return buf;
}

// Frees spans past their watermark.
static void stage_buffer_reclaim(struct nx_vk_stage_buffer *buf, uint64_t current_point) {
	size_t completed = 0;
	struct nx_vk_stage_watermark *mark;
	wl_array_for_each(mark, &buf->watermarks) {
		if (mark->timeline_point > current_point) {
			break;
		}
		buf->tail = mark->head;
		completed++;
	}

	if (completed == 0) {
		return;
	}
	completed *= sizeof(struct nx_vk_stage_watermark);
	if (completed == buf->watermarks.size) {
		buf->watermarks.size = 0;
		return;
	}
	array_remove_at(&buf->watermarks, 0, completed);
}

// Aligned offset or NO_SPACE.
static VkDeviceSize stage_buffer_alloc(struct nx_vk_stage_buffer *buf,
		VkDeviceSize size, VkDeviceSize alignment) {
	VkDeviceSize head = buf->head;
	VkDeviceSize rem = head % alignment;
	if (rem != 0) {
		head += alignment - rem;
	}

	VkDeviceSize end = head >= buf->tail ? buf->buf_size : buf->tail;
	if (head + size < end) {
		buf->head = head + size;
		return head;
	}
	// Wrap around to the start.
	if (size < buf->tail && head >= buf->tail) {
		buf->head = size;
		return 0;
	}
	return NO_SPACE;
}

static VkDeviceSize grown_size(VkDeviceSize max_buf_size, VkDeviceSize size) {
	VkDeviceSize bsize = max_buf_size * 2;
	while (size * 2 > bsize) {
		bsize *= 2;
	}
	if (bsize > max_stage_size) {
		nx_log(NX_INFO, "vulkan stage buffer has reached max size");
		bsize = max_stage_size;
	}
	return bsize;
}

struct nx_vk_buffer_span vulkan_get_stage_span(struct nx_vk_renderer *r,
		VkDeviceSize size, VkDeviceSize alignment) {
	if (size >= max_stage_size) {
		nx_log(NX_ERROR, "cannot allocate stage buffer: "
			"requested size (%zu bytes) exceeds maximum (%zu bytes)",
			(size_t)size, (size_t)max_stage_size - 1);
		return (struct nx_vk_buffer_span){0};
	}

	VkDeviceSize max_buf_size = min_stage_size / 2;
	struct nx_vk_stage_buffer *buf;
	wl_list_for_each(buf, &r->stage.buffers, link) {
		VkDeviceSize offset = stage_buffer_alloc(buf, size, alignment);
		if (offset != NO_SPACE) {
			return (struct nx_vk_buffer_span){ .buffer = buf, .offset = offset, .size = size };
		}
		if (buf->buf_size > max_buf_size) {
			max_buf_size = buf->buf_size;
		}
	}

	buf = stage_buffer_create(r, grown_size(max_buf_size, size));
	if (buf == NULL) {
		return (struct nx_vk_buffer_span){0};
	}
	wl_list_insert(r->stage.buffers.prev, &buf->link);

	VkDeviceSize offset = stage_buffer_alloc(buf, size, alignment);
	assert(offset != NO_SPACE);
	return (struct nx_vk_buffer_span){ .buffer = buf, .offset = offset, .size = size };
}

void vulkan_stage_mark_submit(struct nx_vk_renderer *renderer, uint64_t timeline_point) {
	struct nx_vk_stage_buffer *buf;
	wl_list_for_each(buf, &renderer->stage.buffers, link) {
		if (buf->head == buf->tail) {
			continue;
		}

		struct nx_vk_stage_watermark *mark = wl_array_add(&buf->watermarks, sizeof(*mark));
		if (mark == NULL) {
			nx_log_errno(NX_ERROR, "Allocation failed");
			continue;
		}
		*mark = (struct nx_vk_stage_watermark){
			.head = buf->head,
			.timeline_point = timeline_point,
		};
	}
}

void vulkan_stage_buffer_gc(struct nx_vk_renderer *renderer, uint64_t current_point) {
	struct nx_vk_stage_buffer *buf, *tmp;
	wl_list_for_each_safe(buf, tmp, &renderer->stage.buffers, link) {
		if (buf->head != buf->tail) {
			buf->empty_gc_cnt = 0;
			stage_buffer_reclaim(buf, current_point);
			continue;
		}
		// The first buffer is kept.
		if (buf->buf_size <= min_stage_size) {
			continue;
		}
		if (++buf->empty_gc_cnt >= STAGE_GC_IDLE_LIMIT) {
			stage_buffer_destroy(renderer, buf);
		}
	}
}

void vulkan_stage_buffers_finish(struct nx_vk_renderer *renderer) {
	struct nx_vk_stage_buffer *buf, *tmp;
	wl_list_for_each_safe(buf, tmp, &renderer->stage.buffers, link) {
		stage_buffer_destroy(renderer, buf);
	}
}
