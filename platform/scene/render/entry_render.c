#include <assert.h>
#include "platform/geometry/transform.h"
#include "platform/output/output.h"
#include "platform/render/color.h"
#include "platform/scene/graph/buffer.h"
#include "platform/scene/graph/node.h"
#include "platform/scene/output/scene_output.h"
#include "platform/scene/render/frame.h"

struct entry_target {
	int x, y; // logical, output-local
	struct nx_box box;
	pixman_region32_t clip;
	pixman_region32_t translucent;
	float opacity;
};

static void render_rect(const struct render_data *data,
		const struct nx_scene_rect *scene_rect, const struct entry_target *target) {
	const struct nx_scene_rect_shape *shape = &scene_rect->shape;
	nx_render_pass_add_rect(data->render_pass, &(struct nx_render_rect_options){
		.box = target->box,
		.color = {
			.r = scene_rect->color[0] * target->opacity,
			.g = scene_rect->color[1] * target->opacity,
			.b = scene_rect->color[2] * target->opacity,
			.a = scene_rect->color[3] * target->opacity,
		},
		.clip = &target->clip,
		.corner_radius = (float)shape->radius * data->scale,
		.softness = (float)shape->softness * data->scale,
		.border = (float)shape->border * data->scale,
	});
}

// Rounded clip in buffer pixels.
static struct nx_box output_corner_box(const struct render_data *data,
		const struct nx_scene_buffer *scene_buffer, const struct entry_target *target) {
	struct nx_box box = scene_buffer->corner_box;
	box.x += target->x;
	box.y += target->y;
	scene_transform_output_box(&box, data);
	return box;
}

// Cheaper than sampling a texture.
static void render_single_pixel(const struct render_data *data,
		const struct nx_scene_buffer *scene_buffer, const struct entry_target *target) {
	const uint32_t *color = scene_buffer->single_pixel_buffer_color;
	nx_render_pass_add_rect(data->render_pass, &(struct nx_render_rect_options){
		.box = target->box,
		.color = {
			.r = (float)color[0] / (float)UINT32_MAX,
			.g = (float)color[1] / (float)UINT32_MAX,
			.b = (float)color[2] / (float)UINT32_MAX,
			.a = (float)color[3] / (float)UINT32_MAX *
				scene_buffer->opacity * target->opacity,
		},
		.clip = &target->clip,
		.corner_box = output_corner_box(data, scene_buffer, target),
		.corner_radius = (float)scene_buffer->corner_radius * data->scale,
	});
}

static bool render_texture(const struct render_data *data,
		struct nx_scene_buffer *scene_buffer, const struct entry_target *target) {
	struct nx_texture *texture = scene_buffer_get_texture(scene_buffer,
		data->output->output->renderer);
	if (texture == NULL) {
		scene_output_damage(data->output, &target->clip);
		return false;
	}

	enum wl_output_transform transform =
		nx_output_transform_invert(scene_buffer->transform);
	transform = nx_output_transform_compose(transform, data->transform);

	struct nx_color_primaries primaries = {0};
	if (scene_buffer->primaries != 0) {
		nx_color_primaries_from_named(&primaries, scene_buffer->primaries);
	}

	struct nx_color_luminances src_lum, srgb_lum;
	nx_color_transfer_function_get_default_luminance(
		scene_buffer->transfer_function, &src_lum);
	nx_color_transfer_function_get_default_luminance(
		NX_COLOR_TRANSFER_FUNCTION_SRGB, &srgb_lum);
	float luminance_multiplier = scene_luminance_multiplier(&src_lum, &srgb_lum);
	float alpha = scene_buffer->opacity * target->opacity;

	nx_render_pass_add_texture(data->render_pass, &(struct nx_render_texture_options) {
		.texture = texture,
		.src_box = scene_buffer->src_box,
		.dst_box = target->box,
		.transform = transform,
		.clip = &target->clip,
		.alpha = &alpha,
		.blend_mode = !data->output->scene->calculate_visibility ||
				!pixman_region32_empty(&target->translucent) ?
			NX_RENDER_BLEND_MODE_PREMULTIPLIED : NX_RENDER_BLEND_MODE_NONE,
		.transfer_function = scene_buffer->transfer_function,
		.primaries = scene_buffer->primaries != 0 ? &primaries : NULL,
		.color_encoding = scene_buffer->color_encoding,
		.color_range = scene_buffer->color_range,
		.luminance_multiplier = &luminance_multiplier,
		.corner_box = output_corner_box(data, scene_buffer, target),
		.corner_radius = (float)scene_buffer->corner_radius * data->scale,
		.wait_timeline = scene_buffer->wait_timeline,
		.wait_point = scene_buffer->wait_point,
	});
	return true;
}

// Translucent parts show green.
static void highlight_translucent(const struct render_data *data,
		const struct entry_target *target) {
	nx_render_pass_add_rect(data->render_pass, &(struct nx_render_rect_options){
		.box = target->box,
		.color = { .r = 0, .g = 0.3f, .b = 0, .a = 0.3f },
		.clip = &target->translucent,
	});
}

static void render_buffer(const struct render_data *data,
		struct nx_scene_buffer *scene_buffer, const struct entry_target *target) {
	if (scene_buffer->is_single_pixel_buffer) {
		render_single_pixel(data, scene_buffer, target);
		return;
	}

	if (!render_texture(data, scene_buffer, target)) {
		return;
	}

	struct nx_scene_output_sample_event sample_event = {
		.output = data->output,
		.direct_scanout = false,
		.release_timeline = data->output->in_timeline,
		.release_point = data->output->in_point,
	};
	wl_signal_emit_mutable(&scene_buffer->events.output_sample, &sample_event);

	if (data->output->scene->highlight_transparent_region) {
		highlight_translucent(data, target);
	}
}

void scene_entry_render(const struct render_list_entry *entry,
		const struct render_data *data) {
	struct nx_scene_node *node = entry->node;
	struct entry_target target = { .opacity = entry->opacity };

	pixman_region32_init(&target.clip);
	pixman_region32_copy(&target.clip, &node->visible);
	pixman_region32_translate(&target.clip, -data->logical.x, -data->logical.y);
	scene_logical_to_buffer_outward(&target.clip, data);
	pixman_region32_intersect(&target.clip, &target.clip, &data->damage);
	if (pixman_region32_empty(&target.clip)) {
		pixman_region32_fini(&target.clip);
		return;
	}

	target.x = entry->x - data->logical.x;
	target.y = entry->y - data->logical.y;
	target.box = (struct nx_box){ .x = target.x, .y = target.y };
	scene_node_get_size(node, &target.box.width, &target.box.height);
	scene_transform_output_box(&target.box, data);

	pixman_region32_init(&target.translucent);
	if (entry->opacity == 1) {
		scene_node_opaque_region(node, target.x, target.y, &target.translucent);
		scene_logical_to_buffer(&target.translucent, data);
	}
	pixman_region32_subtract(&target.translucent, &target.clip, &target.translucent);

	switch (node->type) {
	case NX_SCENE_NODE_TREE:
		assert(false);
		break;
	case NX_SCENE_NODE_RECT:
		render_rect(data, nx_scene_rect_from_node(node), &target);
		break;
	case NX_SCENE_NODE_BUFFER:
		render_buffer(data, nx_scene_buffer_from_node(node), &target);
		break;
	}

	pixman_region32_fini(&target.translucent);
	pixman_region32_fini(&target.clip);
}
