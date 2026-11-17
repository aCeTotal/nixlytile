#ifndef PLATFORM_RENDER_PASS_H
#define PLATFORM_RENDER_PASS_H

#include <pixman.h>
#include <stdint.h>
#include <wayland-server-core.h>
#include "platform/render/color.h"
#include "platform/geometry/box.h"

struct nx_renderer;
struct nx_buffer;

/* Draw operations batched until submit */
struct nx_render_pass;

struct nx_buffer_pass_options {
	struct nx_color_transform *color_transform; // NULL encodes gamma 2.2

	/* Signalled on completion, needs features.timeline */
	struct nx_drm_syncobj_timeline *signal_timeline;
	uint64_t signal_point;
};

struct nx_render_pass *nx_renderer_begin_buffer_pass(struct nx_renderer *renderer,
	struct nx_buffer *buffer, const struct nx_buffer_pass_options *options);

/* Consumes the pass */
bool nx_render_pass_submit(struct nx_render_pass *render_pass);

enum nx_render_blend_mode {
	NX_RENDER_BLEND_MODE_PREMULTIPLIED,
	NX_RENDER_BLEND_MODE_NONE,
};

enum nx_scale_filter_mode {
	NX_SCALE_FILTER_BILINEAR,
	NX_SCALE_FILTER_NEAREST,
};

struct nx_render_texture_options {
	struct nx_texture *texture;
	struct nx_fbox src_box; // empty means whole texture
	struct nx_box dst_box; // empty size means texture size
	const float *alpha; // NULL means opaque
	const pixman_region32_t *clip; // NULL disables clipping
	enum wl_output_transform transform;
	enum nx_scale_filter_mode filter_mode;
	enum nx_render_blend_mode blend_mode;
	enum nx_color_transfer_function transfer_function;
	const struct nx_color_primaries *primaries;
	enum nx_color_encoding color_encoding; // YCbCr to RGB matrix
	enum nx_color_range color_range;
	const float *luminance_multiplier; // NULL means 1.0
	struct nx_box corner_box; // rounded clip, buffer pixels
	float corner_radius; // 0 disables the clip

	/* Awaited before sampling, needs features.timeline */
	struct nx_drm_syncobj_timeline *wait_timeline;
	uint64_t wait_point;
};

void nx_render_pass_add_texture(struct nx_render_pass *render_pass,
	const struct nx_render_texture_options *options);

/* Premultiplied, channels in [0, 1] */
struct nx_render_color {
	float r, g, b, a;
};

struct nx_render_rect_options {
	struct nx_box box;
	struct nx_render_color color;
	const pixman_region32_t *clip; // NULL disables clipping
	enum nx_render_blend_mode blend_mode;
	struct nx_box corner_box; // empty means the rect box
	float corner_radius;
	float softness; // feathered edge width
	float border; // outline width, 0 fills
};

void nx_render_pass_add_rect(struct nx_render_pass *render_pass,
	const struct nx_render_rect_options *options);

#endif
