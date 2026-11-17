#ifndef NX_VULKAN_PIPELINE_PIPELINE_H
#define NX_VULKAN_PIPELINE_PIPELINE_H

#include <stdbool.h>
#include <vulkan/vulkan.h>
#include <wayland-util.h>
#include "platform/render/color.h"
#include "platform/render/renderer.h"
#include "platform/vulkan/device/format.h"

struct nx_vk_renderer;
struct nx_vk_render_format_setup;

// One rect per instance.
#define NX_VK_INSTANCE_SIZE (4 * sizeof(float))

struct nx_vk_pipeline_layout_key {
	enum nx_scale_filter_mode filter_mode;

	// YCbCr pipelines only.
	struct {
		const struct nx_vk_format *format;
		enum nx_color_encoding encoding;
		enum nx_color_range range;
	} ycbcr;
};

struct nx_vk_pipeline_layout {
	struct nx_vk_pipeline_layout_key key;

	VkPipelineLayout vk;
	VkDescriptorSetLayout ds;
	VkSampler sampler;
	VkSamplerYcbcrConversion ycbcr_conversion; // YCbCr pipelines only

	struct wl_list link; // nx_vk_renderer.pipeline_layouts
};

// Must match shaders/texture.frag.
enum nx_vk_texture_transform {
	NX_VK_TEXTURE_TRANSFORM_IDENTITY = 0,
	NX_VK_TEXTURE_TRANSFORM_SRGB = 1,
	NX_VK_TEXTURE_TRANSFORM_ST2084_PQ = 2,
	NX_VK_TEXTURE_TRANSFORM_GAMMA22 = 3,
	NX_VK_TEXTURE_TRANSFORM_BT1886 = 4,
};

enum nx_vk_shader_source {
	NX_VK_SHADER_SOURCE_TEXTURE,
	NX_VK_SHADER_SOURCE_SINGLE_COLOR,
};

// Must match shaders/output.frag.
enum nx_vk_output_transform {
	NX_VK_OUTPUT_TRANSFORM_IDENTITY = 0,
	NX_VK_OUTPUT_TRANSFORM_INVERSE_SRGB = 1,
	NX_VK_OUTPUT_TRANSFORM_INVERSE_ST2084_PQ = 2,
	NX_VK_OUTPUT_TRANSFORM_LUT3D = 3,
	NX_VK_OUTPUT_TRANSFORM_INVERSE_GAMMA22 = 4,
	NX_VK_OUTPUT_TRANSFORM_INVERSE_BT1886 = 5,
	NX_VK_OUTPUT_TRANSFORM_COUNT,
};

struct nx_vk_pipeline_key {
	struct nx_vk_pipeline_layout_key layout;
	enum nx_vk_shader_source source;
	enum nx_render_blend_mode blend_mode;

	// Texture source only.
	enum nx_vk_texture_transform texture_transform;
};

struct nx_vk_pipeline {
	struct nx_vk_pipeline_key key;

	VkPipeline vk;
	const struct nx_vk_pipeline_layout *layout;
	struct wl_list link; // nx_vk_render_format_setup.pipelines
};

// Layout matches shaders/common.vert.
struct nx_vk_vert_pcr_data {
	float proj_packed[6];
	float uv_off[2];
	float uv_size[2];
	float padding[2];
};

// Layout matches shaders/texture.frag.
struct nx_vk_frag_texture_pcr_data {
	float matrix[3][4]; // std430 mat3 rows
	float corner_box[4]; // centre, half size
	float alpha;
	float luminance_multiplier;
	float corner_radius;
	float padding;
};

// Layout matches shaders/quad.frag.
struct nx_vk_frag_quad_pcr_data {
	float color[4];
	float corner_box[4]; // centre, half size
	float corner_radius;
	float softness;
	float border;
};

// Layout matches shaders/output.frag.
struct nx_vk_frag_output_pcr_data {
	float matrix[3][4]; // std430 mat3 rows
	float lut_3d_offset;
	float lut_3d_scale;
};

bool vulkan_pipeline_layout_key_equals(const struct nx_vk_pipeline_layout_key *a,
	const struct nx_vk_pipeline_layout_key *b);
struct nx_vk_pipeline_layout *vulkan_get_or_create_pipeline_layout(
	struct nx_vk_renderer *renderer, const struct nx_vk_pipeline_layout_key *key);
void vulkan_pipeline_layouts_finish(struct nx_vk_renderer *renderer);

struct nx_vk_pipeline *vulkan_get_or_create_pipeline(struct nx_vk_render_format_setup *setup,
	const struct nx_vk_pipeline_key *key);
void vulkan_pipeline_set_fixed_state(VkGraphicsPipelineCreateInfo *info);

bool vulkan_init_output_layouts(struct nx_vk_renderer *renderer);
bool vulkan_create_output_pipelines(struct nx_vk_render_format_setup *setup);

bool vulkan_init_static_render_data(struct nx_vk_renderer *renderer);

#endif
