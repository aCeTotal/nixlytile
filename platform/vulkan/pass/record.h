#ifndef NX_VULKAN_PASS_RECORD_H
#define NX_VULKAN_PASS_RECORD_H

#include <pixman.h>
#include <vulkan/vulkan.h>
#include <wayland-server-protocol.h>
#include "platform/geometry/box.h"
#include "platform/vulkan/pass/pass.h"
#include "platform/vulkan/queue/command_buffer.h"
#include "platform/vulkan/queue/stage.h"

static inline void vulkan_pass_bind_pipeline(struct nx_vk_render_pass *pass,
		VkPipeline pipeline) {
	if (pipeline == pass->bound_pipeline) {
		return;
	}
	vkCmdBindPipeline(pass->command_buffer->vk, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
	pass->bound_pipeline = pipeline;
}

// Read by the output subpass.
static inline void vulkan_pass_mark_box_updated(struct nx_vk_render_pass *pass,
		const pixman_box32_t *box) {
	if (!pass->two_pass) {
		return;
	}
	rect_union_add(&pass->updated_region, box);
}

// Instance rect relative to box.
static inline void vulkan_write_rect(float dst[static 4], const pixman_box32_t *rect,
		const struct nx_box *box) {
	dst[0] = (float)(rect->x1 - box->x) / box->width;
	dst[1] = (float)(rect->y1 - box->y) / box->height;
	dst[2] = (float)(rect->x2 - rect->x1) / box->width;
	dst[3] = (float)(rect->y2 - rect->y1) / box->height;
}

void vulkan_pass_project_box(struct nx_vk_render_pass *pass, const struct nx_box *box,
	enum wl_output_transform transform, float matrix[static 9]);
void vulkan_pack_proj_matrix(const float mat3[static 9], float packed[static 6]);
void vulkan_encode_color_matrix(const float mat3[static 9], float mat4[4][4]);

// Box clipped to optional bounds.
void vulkan_pass_clip_region(pixman_region32_t *clip, const pixman_region32_t *bounds,
	const struct nx_box *box);

struct nx_vk_buffer_span vulkan_pass_alloc_instances(struct nx_vk_render_pass *pass,
	int count);
void vulkan_pass_draw_instances(struct nx_vk_render_pass *pass,
	const struct nx_vk_buffer_span *span, int count);

#endif
