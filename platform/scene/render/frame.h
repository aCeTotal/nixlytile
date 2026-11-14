#ifndef PLATFORM_SCENE_RENDER_FRAME_H
#define PLATFORM_SCENE_RENDER_FRAME_H

#include <time.h>
#include "platform/scene/scene.h"

struct nx_buffer;
struct nx_color_luminances;
struct nx_color_transform;
struct nx_output_image_description;
struct nx_output_state;

#define DMABUF_FEEDBACK_DEBOUNCE_FRAMES 30

struct render_data {
	enum wl_output_transform transform;
	float scale;
	struct nx_box logical;
	int width, height; // pending resolution
	int trans_width, trans_height;
	struct timespec now; // damage highlight only

	struct nx_scene_output *output;

	struct nx_render_pass *render_pass;
	pixman_region32_t damage;
};

struct render_list_entry {
	struct nx_scene_node *node;
	int x, y;
	float opacity; // inherited from the trees
};

enum scene_direct_scanout_result {
	SCANOUT_INELIGIBLE,
	SCANOUT_CANDIDATE, // eligible; feedback may fix it
	SCANOUT_SUCCESS,
};

void scene_logical_to_buffer(pixman_region32_t *region, const struct render_data *data);
void scene_logical_to_buffer_outward(pixman_region32_t *region,
	const struct render_data *data);
void scene_transform_output_box(struct nx_box *box, const struct render_data *data);
float scene_luminance_multiplier(const struct nx_color_luminances *src_lum,
	const struct nx_color_luminances *dst_lum);
bool scene_output_combine_color_transforms(struct nx_scene_output *scene_output,
	const struct nx_output_image_description *img_desc,
	struct nx_color_transform *render_gamma);
void scene_output_build_render_list(struct nx_scene_output *scene_output,
	const struct render_data *render_data);
void scene_entry_render(const struct render_list_entry *entry,
	const struct render_data *data);
enum scene_direct_scanout_result scene_entry_try_direct_scanout(
	const struct render_list_entry *entry, struct nx_output_state *state,
	const struct render_data *data);
void scene_buffer_send_scanout_feedback(struct nx_scene_output *scene_output,
	struct nx_scene_buffer *scene_buffer);
void scene_buffer_send_render_feedback(struct nx_scene_output *scene_output,
	struct nx_scene_buffer *scene_buffer);
void scene_debug_damage_prepare(struct nx_scene_output *scene_output,
	struct timespec *now);
void scene_debug_damage_draw(const struct render_data *data,
	const struct nx_buffer *buffer);
void scene_debug_damage_finish(struct nx_scene_output *scene_output);

#endif
