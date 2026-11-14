#include <assert.h>
#include <math.h>
#include "platform/buffer/buffer.h"
#include "platform/geometry/region.h"
#include "platform/output/output.h"
#include "platform/scene/graph/buffer.h"
#include "platform/scene/graph/node.h"
#include "platform/scene/output/scene_output.h"

struct buffer_damage {
	pixman_region32_t region; // cropped to the source box
	float scale_x, scale_y;
	int lx, ly;
	const pixman_region32_t *visible;
};

// Cached: the buffer may go after upload.
static void scene_buffer_cache_single_pixel(struct nx_scene_buffer *scene_buffer,
		struct nx_buffer *buffer) {
	scene_buffer->is_single_pixel_buffer = false;
	if (buffer == NULL) {
		return;
	}

	struct nx_client_buffer *client_buffer = nx_client_buffer_get(buffer);
	if (client_buffer == NULL || client_buffer->source == NULL) {
		return;
	}

	struct nx_single_pixel_buffer_v1 *single_pixel_buffer =
		nx_single_pixel_buffer_v1_try_from_buffer(client_buffer->source);
	if (single_pixel_buffer == NULL) {
		return;
	}

	scene_buffer->is_single_pixel_buffer = true;
	scene_buffer->single_pixel_buffer_color[0] = single_pixel_buffer->r;
	scene_buffer->single_pixel_buffer_color[1] = single_pixel_buffer->g;
	scene_buffer->single_pixel_buffer_color[2] = single_pixel_buffer->b;
	scene_buffer->single_pixel_buffer_color[3] = single_pixel_buffer->a;
}

// Linear filtering bleeds into neighbours.
static int filter_bleed_distance(float output_scale_x, float output_scale_y) {
	float buffer_scale_x = 1.0f / output_scale_x;
	float buffer_scale_y = 1.0f / output_scale_y;
	int dist_x = floor(buffer_scale_x) != buffer_scale_x ?
		(int)ceilf(output_scale_x / 2.0f) : 0;
	int dist_y = floor(buffer_scale_y) != buffer_scale_y ?
		(int)ceilf(output_scale_y / 2.0f) : 0;
	return dist_x >= dist_y ? dist_x : dist_y;
}

static void scene_output_damage_buffer(struct nx_scene_output *scene_output,
		const struct buffer_damage *damage) {
	float output_scale = scene_output->output->scale;
	float output_scale_x = output_scale * damage->scale_x;
	float output_scale_y = output_scale * damage->scale_y;
	pixman_region32_t output_damage;
	pixman_region32_init(&output_damage);
	nx_region_scale_xy(&output_damage, &damage->region,
		output_scale_x, output_scale_y);
	nx_region_expand(&output_damage, &output_damage,
		filter_bleed_distance(output_scale_x, output_scale_y));

	pixman_region32_t cull_region;
	pixman_region32_init(&cull_region);
	pixman_region32_copy(&cull_region, damage->visible);
	scene_region_scale_out(&cull_region, output_scale);
	pixman_region32_translate(&cull_region, (int)(-damage->lx * output_scale),
		(int)(-damage->ly * output_scale));
	pixman_region32_intersect(&output_damage, &output_damage, &cull_region);
	pixman_region32_fini(&cull_region);

	pixman_region32_translate(&output_damage,
		(int)round((damage->lx - scene_output->x) * output_scale),
		(int)round((damage->ly - scene_output->y) * output_scale));
	scene_region_to_buffer_coords(&output_damage, scene_output->output);
	scene_output_damage(scene_output, &output_damage);
	pixman_region32_fini(&output_damage);
}

static void scene_buffer_damage(struct nx_scene_buffer *scene_buffer,
		struct nx_buffer *buffer, const pixman_region32_t *buffer_damage) {
	struct buffer_damage damage = { .visible = &scene_buffer->node.visible };
	if (!nx_scene_node_coords(&scene_buffer->node, &damage.lx, &damage.ly)) {
		return;
	}

	pixman_region32_t fallback_damage;
	pixman_region32_init_rect(&fallback_damage, 0, 0, buffer->width, buffer->height);
	if (!buffer_damage) {
		buffer_damage = &fallback_damage;
	}

	struct nx_fbox box = scene_buffer->src_box;
	if (nx_fbox_empty(&box)) {
		box = (struct nx_fbox){ .width = buffer->width, .height = buffer->height };
	}
	nx_fbox_transform(&box, &box, scene_buffer->transform,
		buffer->width, buffer->height);

	if (scene_buffer->dst_width || scene_buffer->dst_height) {
		damage.scale_x = (float)(scene_buffer->dst_width / box.width);
		damage.scale_y = (float)(scene_buffer->dst_height / box.height);
	} else {
		damage.scale_x = (float)(buffer->width / box.width);
		damage.scale_y = (float)(buffer->height / box.height);
	}

	pixman_region32_init(&damage.region);
	nx_region_transform(&damage.region, buffer_damage,
		scene_buffer->transform, buffer->width, buffer->height);
	pixman_region32_intersect_rect(&damage.region, &damage.region,
		(int)box.x, (int)box.y, (unsigned int)box.width, (unsigned int)box.height);
	pixman_region32_translate(&damage.region, (int)-box.x, (int)-box.y);

	struct nx_scene *scene = scene_node_get_root(&scene_buffer->node);
	struct nx_scene_output *scene_output;
	wl_list_for_each(scene_output, &scene->outputs, link) {
		scene_output_damage_buffer(scene_output, &damage);
	}

	pixman_region32_fini(&damage.region);
	pixman_region32_fini(&fallback_damage);
}

void nx_scene_buffer_set_buffer_with_options(struct nx_scene_buffer *scene_buffer,
		struct nx_buffer *buffer, const struct nx_scene_buffer_set_buffer_options *options) {
	const struct nx_scene_buffer_set_buffer_options default_options = {0};
	if (options == NULL) {
		options = &default_options;
	}

	// Damage needs the buffer size.
	assert(buffer || !options->damage);

	bool mapped = buffer != NULL;
	bool prev_mapped = scene_buffer->buffer != NULL || scene_buffer->texture != NULL;
	if (!mapped && !prev_mapped) {
		return;
	}

	// Remapping or resizing updates the node.
	bool update = mapped != prev_mapped;
	if (buffer != NULL && scene_buffer->dst_width == 0 && scene_buffer->dst_height == 0) {
		update = update || scene_buffer->buffer_width != buffer->width ||
			scene_buffer->buffer_height != buffer->height;
	}

	if (buffer != scene_buffer->buffer) {
		scene_buffer_cache_single_pixel(scene_buffer, buffer);
	}

	scene_buffer_set_buffer(scene_buffer, buffer);
	scene_buffer_set_texture(scene_buffer, NULL);
	scene_buffer_set_wait_timeline(scene_buffer,
		options->wait_timeline, options->wait_point);

	// The update damages the whole node.
	if (update) {
		scene_node_update(&scene_buffer->node, NULL);
		return;
	}

	scene_buffer_damage(scene_buffer, buffer, options->damage);
}

void nx_scene_buffer_set_buffer(struct nx_scene_buffer *scene_buffer,
		struct nx_buffer *buffer) {
	nx_scene_buffer_set_buffer_with_options(scene_buffer, buffer, NULL);
}
