#include <assert.h>
#include <vulkan/vulkan.h>
#include "platform/geometry/matrix.h"
#include "platform/render/color.h"
#include "platform/render/drm_syncobj.h"
#include "platform/vulkan/image/texture.h"
#include "platform/vulkan/pass/pass.h"
#include "platform/vulkan/pass/record.h"
#include "platform/vulkan/pipeline/pipeline.h"
#include "platform/vulkan/pipeline/render_setup.h"
#include "platform/vulkan/queue/command_buffer.h"
#include "platform/vulkan/renderer.h"

struct texture_draw {
	struct nx_vk_texture *texture;
	const struct nx_render_texture_options *options;
	struct nx_fbox src_box;
	struct nx_box dst_box;
	float alpha;
	const pixman_box32_t *rects;
	int rects_len;
};

// Acquired before the render pass.
static void queue_foreign_texture(struct nx_vk_renderer *renderer,
		struct nx_vk_texture *texture) {
	texture->owned = true;
	assert(texture->foreign_link.prev == NULL);
	assert(texture->foreign_link.next == NULL);
	wl_list_insert(&renderer->foreign_textures, &texture->foreign_link);
}

static enum nx_vk_texture_transform texture_transform(enum nx_color_transfer_function tf) {
	switch (tf) {
	case NX_COLOR_TRANSFER_FUNCTION_SRGB:
		return NX_VK_TEXTURE_TRANSFORM_SRGB;
	case NX_COLOR_TRANSFER_FUNCTION_EXT_LINEAR:
		return NX_VK_TEXTURE_TRANSFORM_IDENTITY;
	case NX_COLOR_TRANSFER_FUNCTION_ST2084_PQ:
		return NX_VK_TEXTURE_TRANSFORM_ST2084_PQ;
	case NX_COLOR_TRANSFER_FUNCTION_GAMMA22:
		return NX_VK_TEXTURE_TRANSFORM_GAMMA22;
	case NX_COLOR_TRANSFER_FUNCTION_BT1886:
		return NX_VK_TEXTURE_TRANSFORM_BT1886;
	}
	return NX_VK_TEXTURE_TRANSFORM_IDENTITY;
}

static struct nx_vk_pipeline_key pipeline_key(const struct texture_draw *draw,
		enum nx_vk_texture_transform transform) {
	const struct nx_render_texture_options *options = draw->options;
	const struct nx_vk_texture *texture = draw->texture;
	bool is_ycbcr = vulkan_format_is_ycbcr(texture->format);

	enum nx_color_encoding encoding = options->color_encoding;
	if (is_ycbcr && encoding == NX_COLOR_ENCODING_NONE) {
		encoding = NX_COLOR_ENCODING_BT601;
	}
	enum nx_color_range range = options->color_range;
	if (is_ycbcr && range == NX_COLOR_RANGE_NONE) {
		range = NX_COLOR_RANGE_LIMITED;
	}

	return (struct nx_vk_pipeline_key){
		.source = NX_VK_SHADER_SOURCE_TEXTURE,
		.layout = {
			.ycbcr = {
				.format = is_ycbcr ? texture->format : NULL,
				.encoding = encoding,
				.range = range,
			},
			.filter_mode = options->filter_mode,
		},
		.texture_transform = transform,
		.blend_mode = !texture->has_alpha && draw->alpha == 1.0 ?
			NX_RENDER_BLEND_MODE_NONE : options->blend_mode,
	};
}

static void push_constants(struct nx_vk_render_pass *pass, const struct texture_draw *draw,
		VkPipelineLayout layout) {
	const struct nx_render_texture_options *options = draw->options;
	float matrix[9];
	vulkan_pass_project_box(pass, &draw->dst_box, options->transform, matrix);

	struct nx_vk_vert_pcr_data vert_pcr_data = {
		.uv_off = {
			(float)(draw->src_box.x / options->texture->width),
			(float)(draw->src_box.y / options->texture->height),
		},
		.uv_size = {
			(float)(draw->src_box.width / options->texture->width),
			(float)(draw->src_box.height / options->texture->height),
		},
	};
	vulkan_pack_proj_matrix(matrix, vert_pcr_data.proj_packed);

	float color_matrix[9];
	if (options->primaries != NULL) {
		struct nx_color_primaries srgb;
		nx_color_primaries_from_named(&srgb, NX_COLOR_NAMED_PRIMARIES_SRGB);
		nx_color_primaries_transform_absolute_colorimetric(options->primaries,
			&srgb, color_matrix);
	} else {
		nx_matrix_identity(color_matrix);
	}

	struct nx_vk_frag_texture_pcr_data frag_pcr_data = {
		.alpha = draw->alpha,
		.luminance_multiplier = options->luminance_multiplier != NULL ?
			*options->luminance_multiplier : 1,
	};
	vulkan_encode_color_matrix(color_matrix, frag_pcr_data.matrix);

	VkCommandBuffer cb = pass->command_buffer->vk;
	vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_VERTEX_BIT, 0,
		sizeof(vert_pcr_data), &vert_pcr_data);
	vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(vert_pcr_data),
		sizeof(frag_pcr_data), &frag_pcr_data);
}

// Double precision keeps rects exact.
static void write_rects(struct nx_vk_render_pass *pass, const struct texture_draw *draw,
		float *dst) {
	const struct nx_box *box = &draw->dst_box;
	enum wl_output_transform transform = draw->options->transform;
	for (int i = 0; i < draw->rects_len; i++) {
		const pixman_box32_t *rect = &draw->rects[i];
		vulkan_pass_mark_box_updated(pass, rect);

		struct nx_fbox norm = {
			.x = (double)(rect->x1 - box->x) / box->width,
			.y = (double)(rect->y1 - box->y) / box->height,
			.width = (double)(rect->x2 - rect->x1) / box->width,
			.height = (double)(rect->y2 - rect->y1) / box->height,
		};
		if (transform != WL_OUTPUT_TRANSFORM_NORMAL) {
			nx_fbox_transform(&norm, &norm, transform, 1.0, 1.0);
		}

		dst[i * 4 + 0] = (float)norm.x;
		dst[i * 4 + 1] = (float)norm.y;
		dst[i * 4 + 2] = (float)norm.width;
		dst[i * 4 + 3] = (float)norm.height;
	}
}

// Synced once per pass.
static bool track_pass_texture(struct nx_vk_render_pass *pass, struct nx_vk_texture *texture,
		const struct nx_render_texture_options *options) {
	if (!(texture->dmabuf_imported || options->wait_timeline != NULL) ||
			texture->pass_synced) {
		return true;
	}

	struct nx_vk_render_pass_texture *pass_texture =
		wl_array_add(&pass->textures, sizeof(*pass_texture));
	if (pass_texture == NULL) {
		return false;
	}
	texture->pass_synced = true;

	*pass_texture = (struct nx_vk_render_pass_texture){
		.texture = texture,
	};
	if (options->wait_timeline != NULL) {
		pass_texture->wait_timeline = nx_drm_syncobj_timeline_ref(options->wait_timeline);
		pass_texture->wait_point = options->wait_point;
	}
	return true;
}

static bool draw_texture(struct nx_vk_render_pass *pass, const struct texture_draw *draw) {
	struct nx_vk_texture *texture = draw->texture;
	struct nx_vk_buffer_span span = vulkan_pass_alloc_instances(pass, draw->rects_len);
	if (!span.buffer) {
		return false;
	}

	enum nx_color_transfer_function tf = draw->options->transfer_function;
	if (tf == 0) {
		tf = NX_COLOR_TRANSFER_FUNCTION_GAMMA22;
	}
	// The sampler decodes sRGB views.
	bool srgb_view = tf == NX_COLOR_TRANSFER_FUNCTION_SRGB && texture->using_mutable_srgb;
	enum nx_vk_texture_transform transform =
		srgb_view ? NX_VK_TEXTURE_TRANSFORM_IDENTITY : texture_transform(tf);

	struct nx_vk_pipeline_key key = pipeline_key(draw, transform);
	struct nx_vk_pipeline *pipe = vulkan_get_or_create_pipeline(pass->render_setup, &key);
	if (!pipe) {
		return false;
	}
	struct nx_vk_texture_view *view =
		vulkan_texture_get_or_create_view(texture, pipe->layout, srgb_view);
	if (!view) {
		return false;
	}

	vulkan_pass_bind_pipeline(pass, pipe->vk);
	vkCmdBindDescriptorSets(pass->command_buffer->vk, VK_PIPELINE_BIND_POINT_GRAPHICS,
		pipe->layout->vk, 0, 1, &view->ds, 0, NULL);
	push_constants(pass, draw, pipe->layout->vk);
	write_rects(pass, draw, vulkan_span_map(&span));
	vulkan_pass_draw_instances(pass, &span, draw->rects_len);

	texture->last_used_cb = pass->command_buffer;
	return track_pass_texture(pass, texture, draw->options);
}

void vulkan_render_pass_add_texture(struct nx_render_pass *nx_pass,
		const struct nx_render_texture_options *options) {
	struct nx_vk_render_pass *pass = vulkan_get_render_pass(nx_pass);
	struct nx_vk_texture *texture = vulkan_get_texture(options->texture);
	assert(texture->renderer == pass->renderer);

	if (texture->dmabuf_imported && !texture->owned) {
		queue_foreign_texture(pass->renderer, texture);
	}

	struct texture_draw draw = {
		.texture = texture,
		.options = options,
		.alpha = nx_render_texture_options_get_alpha(options),
	};
	nx_render_texture_options_get_src_box(options, &draw.src_box);
	nx_render_texture_options_get_dst_box(options, &draw.dst_box);

	pixman_region32_t clip;
	vulkan_pass_clip_region(&clip, options->clip, &draw.dst_box);
	draw.rects = pixman_region32_rectangles(&clip, &draw.rects_len);
	if (draw.rects_len > 0 && !draw_texture(pass, &draw)) {
		pass->failed = true;
	}
	pixman_region32_fini(&clip);
}
