#include <math.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include "platform/geometry/matrix.h"
#include "platform/vulkan/pass/record.h"
#include "platform/vulkan/pipeline/pipeline.h"

#define INSTANCE_ALIGNMENT 16

void vulkan_pass_project_box(struct nx_vk_render_pass *pass, const struct nx_box *box,
		enum wl_output_transform transform, float matrix[static 9]) {
	float proj[9];
	nx_matrix_identity(proj);
	nx_matrix_project_box(matrix, box, transform, proj);
	nx_matrix_multiply(matrix, pass->projection, matrix);
}

// Avoids mat3 padding.
void vulkan_pack_proj_matrix(const float mat3[static 9], float packed[static 6]) {
	memcpy(packed, mat3, 6 * sizeof(float));
}

void vulkan_encode_color_matrix(const float mat3[static 9], float rows[3][4]) {
	float padded[3][4] = {
		{ mat3[0], mat3[1], mat3[2], 0 },
		{ mat3[3], mat3[4], mat3[5], 0 },
		{ mat3[6], mat3[7], mat3[8], 0 },
	};
	memcpy(rows, padded, sizeof(padded));
}

void vulkan_encode_corner_box(const struct nx_box *box, float inset, float dst[static 4]) {
	float half_width = (float)box->width / 2;
	float half_height = (float)box->height / 2;
	dst[0] = (float)box->x + half_width;
	dst[1] = (float)box->y + half_height;
	dst[2] = fmaxf(half_width - inset, 0);
	dst[3] = fmaxf(half_height - inset, 0);
}

void vulkan_pass_clip_region(pixman_region32_t *clip, const pixman_region32_t *bounds,
		const struct nx_box *box) {
	if (!bounds) {
		pixman_region32_init_rect(clip, box->x, box->y, box->width, box->height);
		return;
	}
	pixman_region32_init(clip);
	pixman_region32_intersect_rect(clip, bounds, box->x, box->y, box->width, box->height);
}

struct nx_vk_buffer_span vulkan_pass_alloc_instances(struct nx_vk_render_pass *pass,
		int count) {
	return vulkan_get_stage_span(pass->renderer, count * NX_VK_INSTANCE_SIZE,
		INSTANCE_ALIGNMENT);
}

void vulkan_pass_draw_instances(struct nx_vk_render_pass *pass,
		const struct nx_vk_buffer_span *span, int count) {
	VkCommandBuffer cb = pass->command_buffer->vk;
	VkDeviceSize offset = span->offset;
	vkCmdBindVertexBuffers(cb, 0, 1, &span->buffer->buffer, &offset);
	vkCmdDraw(cb, 4, (uint32_t)count, 0, 0);
}
