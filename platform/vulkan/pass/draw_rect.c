#include <math.h>
#include <vulkan/vulkan.h>
#include "platform/vulkan/image/render_buffer.h"
#include "platform/vulkan/pass/pass.h"
#include "platform/vulkan/pass/record.h"
#include "platform/vulkan/pipeline/pipeline.h"
#include "platform/vulkan/pipeline/render_setup.h"
#include "platform/vulkan/queue/command_buffer.h"

struct rect_batch {
	struct nx_box box;
	const pixman_box32_t *rects;
	int len;
	float color[4]; // linear, premultiplied
};

static const struct nx_vk_pipeline_key quad_pipeline_key = {
	.source = NX_VK_SHADER_SOURCE_SINGLE_COLOR,
};

static float color_to_linear(float non_linear) {
	return (float)pow(non_linear, 2.2);
}

static float color_to_linear_premult(float non_linear, float alpha) {
	return (alpha == 0) ? 0 : color_to_linear(non_linear / alpha) * alpha;
}

static void blend_rects(struct nx_vk_render_pass *pass, const struct rect_batch *batch) {
	float matrix[9];
	vulkan_pass_project_box(pass, &batch->box, WL_OUTPUT_TRANSFORM_NORMAL, matrix);

	struct nx_vk_pipeline *pipe =
		vulkan_get_or_create_pipeline(pass->render_setup, &quad_pipeline_key);
	if (!pipe) {
		pass->failed = true;
		return;
	}

	struct nx_vk_buffer_span span = vulkan_pass_alloc_instances(pass, batch->len);
	if (!span.buffer) {
		pass->failed = true;
		return;
	}
	float *instances = vulkan_span_map(&span);
	for (int i = 0; i < batch->len; i++) {
		vulkan_pass_mark_box_updated(pass, &batch->rects[i]);
		vulkan_write_rect(&instances[i * 4], &batch->rects[i], &batch->box);
	}

	struct nx_vk_vert_pcr_data vert_pcr_data = {
		.uv_off = { 0, 0 },
		.uv_size = { 1, 1 },
	};
	vulkan_pack_proj_matrix(matrix, vert_pcr_data.proj_packed);

	VkCommandBuffer cb = pass->command_buffer->vk;
	vulkan_pass_bind_pipeline(pass, pipe->vk);
	vkCmdPushConstants(cb, pipe->layout->vk, VK_SHADER_STAGE_VERTEX_BIT, 0,
		sizeof(vert_pcr_data), &vert_pcr_data);
	vkCmdPushConstants(cb, pipe->layout->vk, VK_SHADER_STAGE_FRAGMENT_BIT,
		sizeof(vert_pcr_data), sizeof(batch->color), batch->color);
	vulkan_pass_draw_instances(pass, &span, batch->len);
}

static void clear_rects(struct nx_vk_render_pass *pass, const struct rect_batch *batch) {
	VkClearAttachment clear_att = {
		.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
		.colorAttachment = 0,
		.clearValue.color.float32 = {
			batch->color[0], batch->color[1], batch->color[2], batch->color[3],
		},
	};
	VkClearRect clear_rect = {
		.layerCount = 1,
	};
	for (int i = 0; i < batch->len; i++) {
		const pixman_box32_t *rect = &batch->rects[i];
		vulkan_pass_mark_box_updated(pass, rect);
		clear_rect.rect = (VkRect2D){
			.offset = { .x = rect->x1, .y = rect->y1 },
			.extent = { .width = rect->x2 - rect->x1, .height = rect->y2 - rect->y1 },
		};
		vkCmdClearAttachments(pass->command_buffer->vk, 1, &clear_att, 1, &clear_rect);
	}
}

static void draw_rects(struct nx_vk_render_pass *pass, const struct rect_batch *batch,
		enum nx_render_blend_mode blend_mode) {
	switch (blend_mode) {
	case NX_RENDER_BLEND_MODE_PREMULTIPLIED:
		blend_rects(pass, batch);
		break;
	case NX_RENDER_BLEND_MODE_NONE:
		clear_rects(pass, batch);
		break;
	}
}

void vulkan_render_pass_add_rect(struct nx_render_pass *nx_pass,
		const struct nx_render_rect_options *options) {
	struct nx_vk_render_pass *pass = vulkan_get_render_pass(nx_pass);

	// Shaders blend in linear space.
	const struct nx_render_color *color = &options->color;
	struct rect_batch batch = {
		.color = {
			color_to_linear_premult(color->r, color->a),
			color_to_linear_premult(color->g, color->a),
			color_to_linear_premult(color->b, color->a),
			color->a,
		},
	};
	nx_render_rect_options_get_box(options, pass->render_buffer->nx_buffer, &batch.box);

	pixman_region32_t clip;
	vulkan_pass_clip_region(&clip, options->clip, &batch.box);
	batch.rects = pixman_region32_rectangles(&clip, &batch.len);

	if (batch.len > 0) {
		draw_rects(pass, &batch, options->blend_mode);
	}
	pixman_region32_fini(&clip);
}
