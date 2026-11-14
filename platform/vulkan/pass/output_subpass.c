#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include "platform/buffer/buffer.h"
#include "platform/geometry/matrix.h"
#include "platform/vulkan/image/render_buffer.h"
#include "platform/vulkan/pass/color_transform.h"
#include "platform/vulkan/pass/pass.h"
#include "platform/vulkan/pass/record.h"
#include "platform/vulkan/pipeline/pipeline.h"
#include "platform/vulkan/pipeline/render_setup.h"
#include "platform/vulkan/queue/command_buffer.h"
#include "platform/vulkan/renderer.h"

static enum nx_vk_output_transform output_transform(
		const struct nx_vk_color_transform *transform) {
	if (transform && transform->lut_3d.dim > 0) {
		return NX_VK_OUTPUT_TRANSFORM_LUT3D;
	}

	enum nx_color_transfer_function tf = transform ?
		transform->inverse_eotf : NX_COLOR_TRANSFER_FUNCTION_GAMMA22;
	switch (tf) {
	case NX_COLOR_TRANSFER_FUNCTION_EXT_LINEAR:
		return NX_VK_OUTPUT_TRANSFORM_IDENTITY;
	case NX_COLOR_TRANSFER_FUNCTION_SRGB:
		return NX_VK_OUTPUT_TRANSFORM_INVERSE_SRGB;
	case NX_COLOR_TRANSFER_FUNCTION_ST2084_PQ:
		return NX_VK_OUTPUT_TRANSFORM_INVERSE_ST2084_PQ;
	case NX_COLOR_TRANSFER_FUNCTION_GAMMA22:
		return NX_VK_OUTPUT_TRANSFORM_INVERSE_GAMMA22;
	case NX_COLOR_TRANSFER_FUNCTION_BT1886:
		return NX_VK_OUTPUT_TRANSFORM_INVERSE_BT1886;
	}
	abort(); // unreachable
}

static void push_constants(struct nx_vk_render_pass *pass, const struct nx_box *box,
		const struct nx_vk_color_transform *transform) {
	struct nx_vk_renderer *renderer = pass->renderer;
	float matrix[9];
	vulkan_pass_project_box(pass, box, WL_OUTPUT_TRANSFORM_NORMAL, matrix);
	struct nx_vk_vert_pcr_data vert_pcr_data = {
		.uv_off = { 0, 0 },
		.uv_size = { 1, 1 },
	};
	vulkan_pack_proj_matrix(matrix, vert_pcr_data.proj_packed);

	bool need_lut = transform && transform->lut_3d.dim > 0;
	size_t dim = need_lut ? transform->lut_3d.dim : 1;
	float color_matrix[9];
	if (transform && !need_lut) {
		memcpy(color_matrix, transform->color_matrix, sizeof(color_matrix));
	} else {
		nx_matrix_identity(color_matrix);
	}

	struct nx_vk_frag_output_pcr_data frag_pcr_data = {
		.lut_3d_offset = 0.5f / dim,
		.lut_3d_scale = (float)(dim - 1) / dim,
	};
	vulkan_encode_color_matrix(color_matrix, frag_pcr_data.matrix);

	VkCommandBuffer cb = pass->command_buffer->vk;
	vkCmdPushConstants(cb, renderer->output_pipe_layout, VK_SHADER_STAGE_VERTEX_BIT, 0,
		sizeof(vert_pcr_data), &vert_pcr_data);
	vkCmdPushConstants(cb, renderer->output_pipe_layout, VK_SHADER_STAGE_FRAGMENT_BIT,
		sizeof(vert_pcr_data), sizeof(frag_pcr_data), &frag_pcr_data);
}

// Encodes blend image for output.
bool vulkan_record_output_subpass(struct nx_vk_render_pass *pass) {
	struct nx_vk_renderer *renderer = pass->renderer;
	VkCommandBuffer cb = pass->command_buffer->vk;
	vkCmdNextSubpass(cb, VK_SUBPASS_CONTENTS_INLINE);

	const struct nx_vk_color_transform *transform = NULL;
	if (pass->color_transform != NULL) {
		transform = vulkan_get_color_transform(pass->color_transform, renderer);
		assert(transform);
	}

	struct nx_buffer *buffer = pass->render_buffer->nx_buffer;
	struct nx_box box = { 0, 0, buffer->width, buffer->height };
	enum nx_vk_output_transform output = output_transform(transform);
	vulkan_pass_bind_pipeline(pass, pass->render_setup->output_pipes[output]);
	push_constants(pass, &box, transform);

	VkDescriptorSet ds[] = {
		pass->render_buffer->blend.ds,
		output == NX_VK_OUTPUT_TRANSFORM_LUT3D ?
			transform->lut_3d.ds : renderer->dummy_lut3d.ds,
	};
	vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer->output_pipe_layout,
		0, sizeof(ds) / sizeof(ds[0]), ds, 0, NULL);

	const pixman_region32_t *clip = rect_union_evaluate(&pass->updated_region);
	int rects_len;
	const pixman_box32_t *rects = pixman_region32_rectangles(clip, &rects_len);
	if (rects_len == 0) {
		return true;
	}

	struct nx_vk_buffer_span span = vulkan_pass_alloc_instances(pass, rects_len);
	if (!span.buffer) {
		return false;
	}
	float *instances = vulkan_span_map(&span);
	for (int i = 0; i < rects_len; i++) {
		vulkan_write_rect(&instances[i * 4], &rects[i], &box);
	}
	vulkan_pass_draw_instances(pass, &span, rects_len);
	return true;
}
