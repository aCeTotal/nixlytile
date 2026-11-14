#include <assert.h>
#include <stdlib.h>
#include <vulkan/vulkan.h>
#include "platform/buffer/buffer.h"
#include "platform/geometry/matrix.h"
#include "platform/log/log.h"
#include "platform/render/color.h"
#include "platform/render/drm_syncobj.h"
#include "platform/vulkan/image/render_buffer.h"
#include "platform/vulkan/image/texture.h"
#include "platform/vulkan/pass/color_transform.h"
#include "platform/vulkan/pass/pass.h"
#include "platform/vulkan/queue/command_buffer.h"
#include "platform/vulkan/renderer.h"

static const struct nx_render_pass_impl render_pass_impl = {
	.submit = vulkan_render_pass_submit,
	.add_rect = vulkan_render_pass_add_rect,
	.add_texture = vulkan_render_pass_add_texture,
};

static const struct nx_vk_image_use undefined = {
	.layout = VK_IMAGE_LAYOUT_UNDEFINED,
	.stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
	.access = 0,
};

static const struct nx_vk_image_use lut_sampled = {
	.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	.stage = VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
	.access = VK_ACCESS_SHADER_READ_BIT,
};

struct nx_vk_render_pass *vulkan_get_render_pass(struct nx_render_pass *nx_pass) {
	assert(nx_pass->impl == &render_pass_impl);
	struct nx_vk_render_pass *pass = wl_container_of(nx_pass, pass, base);
	return pass;
}

void vulkan_render_pass_destroy(struct nx_vk_render_pass *pass) {
	struct nx_vk_render_pass_texture *pass_texture;
	wl_array_for_each(pass_texture, &pass->textures) {
		pass_texture->texture->pass_synced = false;
		nx_drm_syncobj_timeline_unref(pass_texture->wait_timeline);
	}

	nx_color_transform_unref(pass->color_transform);
	nx_drm_syncobj_timeline_unref(pass->signal_timeline);
	rect_union_finish(&pass->updated_region);
	wl_array_release(&pass->textures);
	free(pass);
}

// Zero when not an EOTF.
static uint32_t output_inverse_eotf(struct nx_color_transform *transform) {
	if (transform == NULL) {
		return NX_COLOR_TRANSFER_FUNCTION_GAMMA22;
	}
	if (transform->type != COLOR_TRANSFORM_INVERSE_EOTF) {
		return 0;
	}
	return nx_color_transform_inverse_eotf_from_base(transform)->tf;
}

static enum nx_vk_pathway pick_pathway(const struct nx_vk_render_buffer *buffer,
		const struct nx_buffer_pass_options *options) {
	uint32_t inv_eotf = output_inverse_eotf(options->color_transform);
	if (inv_eotf == NX_COLOR_TRANSFER_FUNCTION_EXT_LINEAR) {
		return NX_VK_PATHWAY_LINEAR;
	}
	if (inv_eotf == NX_COLOR_TRANSFER_FUNCTION_SRGB &&
			buffer->targets[NX_VK_PATHWAY_SRGB].framebuffer != VK_NULL_HANDLE) {
		return NX_VK_PATHWAY_SRGB;
	}
	return NX_VK_PATHWAY_TWO_PASS;
}

static bool setup_target(struct nx_vk_render_buffer *buffer, enum nx_vk_pathway pathway) {
	struct nx_dmabuf_attributes attribs;
	nx_buffer_get_dmabuf(buffer->nx_buffer, &attribs);
	bool ok = pathway == NX_VK_PATHWAY_TWO_PASS ?
		vulkan_setup_two_pass_framebuffer(buffer, &attribs) :
		vulkan_setup_one_pass_framebuffer(buffer, &attribs, pathway);
	if (!ok) {
		nx_log(NX_ERROR, "Failed to set up blend image");
	}
	return ok;
}

static bool prepare_target(struct nx_vk_render_buffer *buffer,
		const struct nx_buffer_pass_options *options, enum nx_vk_pathway pathway) {
	if (pathway == NX_VK_PATHWAY_TWO_PASS &&
			!vulkan_ensure_color_transform(buffer->renderer, options->color_transform)) {
		return false;
	}
	if (pathway == NX_VK_PATHWAY_SRGB || buffer->targets[pathway].image_view) {
		return true;
	}
	return setup_target(buffer, pathway);
}

static struct nx_vk_command_buffer *begin_command_buffer(struct nx_vk_renderer *renderer) {
	struct nx_vk_command_buffer *cb = vulkan_acquire_command_buffer(renderer);
	if (cb == NULL) {
		return NULL;
	}

	VkCommandBufferBeginInfo begin_info = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};
	VkResult res = vkBeginCommandBuffer(cb->vk, &begin_info);
	if (res != VK_SUCCESS) {
		nx_vk_error("vkBeginCommandBuffer", res);
		vulkan_reset_command_buffer(cb);
		return NULL;
	}
	return cb;
}

static void record_begin(struct nx_vk_render_pass *pass) {
	struct nx_vk_renderer *renderer = pass->renderer;
	VkCommandBuffer cb = pass->command_buffer->vk;
	if (!renderer->dummy_lut3d_transitioned) {
		renderer->dummy_lut3d_transitioned = true;
		vulkan_change_layout(cb, renderer->dummy_lut3d.image, undefined, lut_sampled);
	}

	struct nx_buffer *buffer = pass->render_buffer->nx_buffer;
	VkRect2D rect = { .extent = { buffer->width, buffer->height } };

	// First use clears blending.
	bool blend_first_use = pass->two_pass && !pass->render_buffer->blend.transitioned;
	VkClearValue clear_value = {0};
	VkRenderPassBeginInfo rp_info = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderArea = rect,
		.clearValueCount = blend_first_use ? 1 : 0,
		.pClearValues = blend_first_use ? &clear_value : NULL,
		.renderPass = blend_first_use ?
			pass->render_setup->render_pass_clear : pass->render_setup->render_pass,
		.framebuffer = pass->render_target->framebuffer,
	};
	vkCmdBeginRenderPass(cb, &rp_info, VK_SUBPASS_CONTENTS_INLINE);

	vkCmdSetViewport(cb, 0, 1, &(VkViewport){
		.width = buffer->width,
		.height = buffer->height,
		.maxDepth = 1,
	});
	vkCmdSetScissor(cb, 0, 1, &rect);

	// GL-style projection, flipped for Vulkan.
	matrix_projection(pass->projection, buffer->width, buffer->height,
		WL_OUTPUT_TRANSFORM_FLIPPED_180);
}

struct nx_vk_render_pass *vulkan_begin_render_pass(struct nx_vk_renderer *renderer,
		struct nx_vk_render_buffer *buffer, const struct nx_buffer_pass_options *options) {
	enum nx_vk_pathway pathway = pick_pathway(buffer, options);
	if (!prepare_target(buffer, options, pathway)) {
		return NULL;
	}

	struct nx_vk_render_pass *pass = calloc(1, sizeof(*pass));
	if (pass == NULL) {
		return NULL;
	}
	nx_render_pass_init(&pass->base, &render_pass_impl);
	pass->renderer = renderer;
	pass->two_pass = pathway == NX_VK_PATHWAY_TWO_PASS;
	if (options->color_transform != NULL) {
		pass->color_transform = nx_color_transform_ref(options->color_transform);
	}
	if (options->signal_timeline != NULL) {
		pass->signal_timeline = nx_drm_syncobj_timeline_ref(options->signal_timeline);
		pass->signal_point = options->signal_point;
	}
	rect_union_init(&pass->updated_region);

	pass->command_buffer = begin_command_buffer(renderer);
	if (pass->command_buffer == NULL) {
		vulkan_render_pass_destroy(pass);
		return NULL;
	}

	pass->render_buffer = buffer;
	pass->render_target = &buffer->targets[pathway];
	pass->render_setup = pass->render_target->render_setup;
	record_begin(pass);
	nx_buffer_lock(buffer->nx_buffer);
	return pass;
}
