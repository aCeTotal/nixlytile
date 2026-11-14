#include <math.h>
#include "platform/geometry/region.h"
#include "platform/geometry/transform.h"
#include "platform/output/damage_ring.h"
#include "platform/output/output.h"
#include "platform/scene/output/scene_output.h"

// Rounds fractional edges outward.
void scene_region_scale_out(pixman_region32_t *region, float scale) {
	nx_region_scale(region, region, scale);

	if (floor(scale) != scale) {
		nx_region_expand(region, region, 1);
	}
}

void scene_region_to_buffer_coords(pixman_region32_t *damage, struct nx_output *output) {
	int width, height;
	nx_output_transformed_resolution(output, &width, &height);

	nx_region_transform(damage, damage,
		nx_output_transform_invert(output->transform), width, height);
}

void scene_output_damage(struct nx_scene_output *scene_output,
		const pixman_region32_t *damage) {
	struct nx_output *output = scene_output->output;

	pixman_region32_t clipped;
	pixman_region32_init(&clipped);
	pixman_region32_intersect_rect(&clipped, damage, 0, 0, output->width, output->height);

	if (!pixman_region32_empty(&clipped)) {
		nx_output_schedule_frame(scene_output->output);
		nx_damage_ring_add(&scene_output->damage_ring, &clipped);

		pixman_region32_union(&scene_output->pending_commit_damage,
			&scene_output->pending_commit_damage, &clipped);
	}

	pixman_region32_fini(&clipped);
}

void scene_output_damage_whole(struct nx_scene_output *scene_output) {
	struct nx_output *output = scene_output->output;

	pixman_region32_t damage;
	pixman_region32_init_rect(&damage, 0, 0, output->width, output->height);
	scene_output_damage(scene_output, &damage);
	pixman_region32_fini(&damage);
}

void scene_damage_outputs(struct nx_scene *scene, const pixman_region32_t *damage) {
	if (pixman_region32_empty(damage)) {
		return;
	}

	struct nx_scene_output *scene_output;
	wl_list_for_each(scene_output, &scene->outputs, link) {
		pixman_region32_t output_damage;
		pixman_region32_init(&output_damage);
		pixman_region32_copy(&output_damage, damage);
		pixman_region32_translate(&output_damage,
			-scene_output->x, -scene_output->y);
		scene_region_scale_out(&output_damage, scene_output->output->scale);
		scene_region_to_buffer_coords(&output_damage, scene_output->output);
		scene_output_damage(scene_output, &output_damage);
		pixman_region32_fini(&output_damage);
	}
}
