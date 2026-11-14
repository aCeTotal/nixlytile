#ifndef PLATFORM_SCENE_OUTPUT_SCENE_OUTPUT_H
#define PLATFORM_SCENE_OUTPUT_SCENE_OUTPUT_H

#include "platform/scene/scene.h"

void scene_region_scale_out(pixman_region32_t *region, float scale);
void scene_region_to_buffer_coords(pixman_region32_t *damage, struct nx_output *output);
void scene_output_damage(struct nx_scene_output *scene_output,
	const pixman_region32_t *damage);
void scene_output_damage_whole(struct nx_scene_output *scene_output);
void scene_damage_outputs(struct nx_scene *scene, const pixman_region32_t *damage);
struct nx_color_transform *scene_output_render_gamma(struct nx_scene_output *scene_output);
void scene_output_state_attempt_gamma(struct nx_scene_output *scene_output,
	struct nx_output_state *state);

#endif
