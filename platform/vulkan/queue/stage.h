#ifndef NX_VULKAN_QUEUE_STAGE_H
#define NX_VULKAN_QUEUE_STAGE_H

#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>
#include <wayland-util.h>

struct nx_vk_renderer;

struct nx_vk_stage_watermark {
	VkDeviceSize head;
	uint64_t timeline_point;
};

// Ring buffer for staging transfers.
struct nx_vk_stage_buffer {
	struct wl_list link; // nx_vk_renderer.stage.buffers
	VkBuffer buffer;
	VkDeviceMemory memory;
	VkDeviceSize buf_size;
	void *cpu_mapping;

	VkDeviceSize head;
	VkDeviceSize tail;

	struct wl_array watermarks; // nx_vk_stage_watermark
	int empty_gc_cnt;
};

// Range within a ring buffer.
struct nx_vk_buffer_span {
	struct nx_vk_stage_buffer *buffer;
	VkDeviceSize offset;
	VkDeviceSize size;
};

// Mapped span, released after vulkan_stage_mark_submit.
struct nx_vk_buffer_span vulkan_get_stage_span(struct nx_vk_renderer *renderer,
	VkDeviceSize size, VkDeviceSize alignment);

static inline void *vulkan_span_map(const struct nx_vk_buffer_span *span) {
	return (char *)span->buffer->cpu_mapping + span->offset;
}

// Frees current spans after timeline_point.
void vulkan_stage_mark_submit(struct nx_vk_renderer *renderer, uint64_t timeline_point);
void vulkan_stage_buffer_gc(struct nx_vk_renderer *renderer, uint64_t current_point);
void vulkan_stage_buffers_finish(struct nx_vk_renderer *renderer);

// Runs before the next frame.
VkCommandBuffer vulkan_record_stage_cb(struct nx_vk_renderer *renderer);
// Submits and blocks until done.
bool vulkan_submit_stage_wait(struct nx_vk_renderer *renderer);

#endif
