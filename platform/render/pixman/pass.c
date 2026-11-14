#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include "platform/render/pixman/pixman_renderer.h"

/* Exact for right-angle transforms */
struct rotation {
	int cos, sin;
	int x, y; // origin shift after rotating
};

struct blit_geometry {
	struct nx_box src; // rounded crop
	struct nx_box src_rotated; // src in dst orientation
	struct nx_box dst;
};

static const struct nx_render_pass_impl render_pass_impl;

static struct nx_pixman_render_pass *get_render_pass(struct nx_render_pass *nx_pass) {
	assert(nx_pass->impl == &render_pass_impl);
	struct nx_pixman_render_pass *pass = wl_container_of(nx_pass, pass, base);
	return pass;
}

static bool render_pass_submit(struct nx_render_pass *nx_pass) {
	struct nx_pixman_render_pass *pass = get_render_pass(nx_pass);

	nx_buffer_end_data_ptr_access(pass->buffer->buffer);
	nx_buffer_unlock(pass->buffer->buffer);
	free(pass);

	return true;
}

static int round_to_int(double v) {
	return (int)roundf((float)v);
}

static uint16_t to_pixman_channel(float v) {
	return (uint16_t)(v * 0xFFFF);
}

static pixman_op_t get_pixman_blending(enum nx_render_blend_mode mode) {
	switch (mode) {
	case NX_RENDER_BLEND_MODE_PREMULTIPLIED:
		return PIXMAN_OP_OVER;
	case NX_RENDER_BLEND_MODE_NONE:
		return PIXMAN_OP_SRC;
	}
	abort();
}

static struct rotation transform_rotation(enum wl_output_transform transform,
		const struct nx_box *src) {
	switch (transform) {
	case WL_OUTPUT_TRANSFORM_90:
	case WL_OUTPUT_TRANSFORM_FLIPPED_90:
		return (struct rotation){ .cos = 0, .sin = 1, .y = src->width };
	case WL_OUTPUT_TRANSFORM_180:
	case WL_OUTPUT_TRANSFORM_FLIPPED_180:
		return (struct rotation){ .cos = -1, .sin = 0, .x = src->width, .y = src->height };
	case WL_OUTPUT_TRANSFORM_270:
	case WL_OUTPUT_TRANSFORM_FLIPPED_270:
		return (struct rotation){ .cos = 0, .sin = -1, .x = src->height };
	case WL_OUTPUT_TRANSFORM_NORMAL:
	case WL_OUTPUT_TRANSFORM_FLIPPED:
		break;
	}
	return (struct rotation){ .cos = 1, .sin = 0 };
}

/* Pixman maps dst to src */
static void set_texture_transform(pixman_image_t *image,
		enum wl_output_transform transform, const struct blit_geometry *geo) {
	struct rotation rot = transform_rotation(transform, &geo->src);

	struct pixman_transform matrix;
	pixman_transform_init_identity(&matrix);

	pixman_transform_scale(&matrix, NULL,
		pixman_double_to_fixed(geo->src_rotated.width / (double)geo->dst.width),
		pixman_double_to_fixed(geo->src_rotated.height / (double)geo->dst.height));

	pixman_transform_translate(&matrix, NULL,
		-pixman_int_to_fixed(rot.x), -pixman_int_to_fixed(rot.y));

	pixman_transform_rotate(&matrix, NULL,
		pixman_int_to_fixed(rot.cos), pixman_int_to_fixed(rot.sin));

	if (transform >= WL_OUTPUT_TRANSFORM_FLIPPED) {
		pixman_transform_translate(&matrix, NULL,
			-pixman_int_to_fixed(geo->src.width), pixman_int_to_fixed(0));
		pixman_transform_scale(&matrix, NULL,
			pixman_int_to_fixed(-1), pixman_int_to_fixed(1));
	}

	pixman_transform_translate(&matrix, NULL,
		pixman_int_to_fixed(geo->src.x), pixman_int_to_fixed(geo->src.y));

	pixman_image_set_transform(image, &matrix);
}

static void set_texture_filter(pixman_image_t *image, enum nx_scale_filter_mode mode) {
	switch (mode) {
	case NX_SCALE_FILTER_BILINEAR:
		pixman_image_set_repeat(image, PIXMAN_REPEAT_PAD);
		pixman_image_set_filter(image, PIXMAN_FILTER_BILINEAR, NULL, 0);
		break;
	case NX_SCALE_FILTER_NEAREST:
		pixman_image_set_filter(image, PIXMAN_FILTER_NEAREST, NULL, 0);
		break;
	}
}

static void get_blit_geometry(const struct nx_render_texture_options *options,
		const struct nx_buffer *target, struct blit_geometry *geo) {
	struct nx_fbox src_fbox;
	nx_render_texture_options_get_src_box(options, &src_fbox);
	geo->src = (struct nx_box){
		.x = round_to_int(src_fbox.x),
		.y = round_to_int(src_fbox.y),
		.width = round_to_int(src_fbox.width),
		.height = round_to_int(src_fbox.height),
	};
	nx_render_texture_options_get_dst_box(options, &geo->dst);
	nx_box_transform(&geo->src_rotated, &geo->src, options->transform,
		target->width, target->height);
}

static void render_pass_add_texture(struct nx_render_pass *nx_pass,
		const struct nx_render_texture_options *options) {
	struct nx_pixman_render_pass *pass = get_render_pass(nx_pass);
	struct nx_pixman_texture *texture = nx_pixman_texture_get(options->texture);
	struct nx_pixman_buffer *buffer = pass->buffer;

	if (!begin_pixman_data_ptr_access(texture->buffer,
			&texture->image, NX_BUFFER_DATA_PTR_ACCESS_READ)) {
		return;
	}

	pixman_op_t op = get_pixman_blending(options->blend_mode);
	pixman_image_set_clip_region32(buffer->image, options->clip);

	struct blit_geometry geo;
	get_blit_geometry(options, buffer->buffer, &geo);

	pixman_image_t *mask = NULL;
	float alpha = nx_render_texture_options_get_alpha(options);
	if (alpha != 1) {
		mask = pixman_image_create_solid_fill(&(struct pixman_color){
			.alpha = to_pixman_channel(alpha),
		});
	}

	if (options->transform != WL_OUTPUT_TRANSFORM_NORMAL ||
			geo.src_rotated.width != geo.dst.width ||
			geo.src_rotated.height != geo.dst.height) {
		set_texture_transform(texture->image, options->transform, &geo);
		set_texture_filter(texture->image, options->filter_mode);
		// Scaled dst size crops
		pixman_image_composite32(op, texture->image, mask, buffer->image,
			0, 0, 0, 0, geo.dst.x, geo.dst.y, geo.dst.width, geo.dst.height);
		pixman_image_set_transform(texture->image, NULL);
	} else {
		pixman_image_set_transform(texture->image, NULL);
		pixman_image_composite32(op, texture->image, mask, buffer->image,
			geo.src.x, geo.src.y, 0, 0, geo.dst.x, geo.dst.y,
			geo.src.width, geo.src.height);
	}

	pixman_image_set_clip_region32(buffer->image, NULL);

	nx_buffer_end_data_ptr_access(texture->buffer);

	if (mask != NULL) {
		pixman_image_unref(mask);
	}
}

static void render_pass_add_rect(struct nx_render_pass *nx_pass,
		const struct nx_render_rect_options *options) {
	struct nx_pixman_render_pass *pass = get_render_pass(nx_pass);
	struct nx_pixman_buffer *buffer = pass->buffer;
	struct nx_box box;
	nx_render_rect_options_get_box(options, pass->buffer->buffer, &box);

	pixman_op_t op = get_pixman_blending(options->color.a == 1 ?
		NX_RENDER_BLEND_MODE_NONE : options->blend_mode);

	struct pixman_color color = {
		.red = to_pixman_channel(options->color.r),
		.green = to_pixman_channel(options->color.g),
		.blue = to_pixman_channel(options->color.b),
		.alpha = to_pixman_channel(options->color.a),
	};

	pixman_image_t *fill = pixman_image_create_solid_fill(&color);

	pixman_image_set_clip_region32(buffer->image, options->clip);
	pixman_image_composite32(op, fill, NULL, buffer->image,
		0, 0, 0, 0, box.x, box.y, box.width, box.height);
	pixman_image_set_clip_region32(buffer->image, NULL);

	pixman_image_unref(fill);
}

static const struct nx_render_pass_impl render_pass_impl = {
	.submit = render_pass_submit,
	.add_texture = render_pass_add_texture,
	.add_rect = render_pass_add_rect,
};

struct nx_pixman_render_pass *begin_pixman_render_pass(
		struct nx_pixman_buffer *buffer) {
	struct nx_pixman_render_pass *pass = calloc(1, sizeof(*pass));
	if (pass == NULL) {
		return NULL;
	}

	nx_render_pass_init(&pass->base, &render_pass_impl);

	if (!begin_pixman_data_ptr_access(buffer->buffer, &buffer->image,
			NX_BUFFER_DATA_PTR_ACCESS_READ | NX_BUFFER_DATA_PTR_ACCESS_WRITE)) {
		free(pass);
		return NULL;
	}

	nx_buffer_lock(buffer->buffer);
	pass->buffer = buffer;

	return pass;
}
