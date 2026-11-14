#ifndef PLATFORM_SCENE_OUTPUT_VIEWPORT_H
#define PLATFORM_SCENE_OUTPUT_VIEWPORT_H

#include <pixman.h>
#include <stdint.h>
#include <time.h>
#include <wayland-server-core.h>
#include "platform/container/addon.h"
#include "platform/output/damage_ring.h"

struct nx_output;
struct nx_output_state;
struct nx_scene;

struct nx_scene_output {
	struct nx_output *output;
	struct wl_list link; // nx_scene.outputs
	struct nx_scene *scene;
	struct nx_addon addon;

	struct nx_damage_ring damage_ring;

	int x, y;

	struct {
		pixman_region32_t pending_commit_damage;

		uint8_t index;

		// Frames toward scanout feedback.
		uint8_t dmabuf_feedback_debounce;
		bool prev_scanout;

		bool gamma_lut_changed;
		struct nx_gamma_control_v1 *gamma_lut;
		struct nx_color_transform *gamma_lut_color_transform;

		struct nx_color_transform *prev_gamma_lut_color_transform;
		struct nx_color_transform *combined_color_transform;

		struct wl_listener output_commit;
		struct wl_listener output_damage;
		struct wl_listener output_needs_frame;

		struct wl_list damage_highlight_regions;

		struct wl_array render_list;

		struct nx_drm_syncobj_timeline *in_timeline;
		uint64_t in_point;
		struct nx_drm_syncobj_timeline *out_timeline;
		uint64_t out_point;
	};
};

struct nx_scene_output *nx_scene_output_create(struct nx_scene *scene,
	struct nx_output *output);
void nx_scene_output_destroy(struct nx_scene_output *scene_output);
void nx_scene_output_set_position(struct nx_scene_output *scene_output,
	int lx, int ly);
bool nx_scene_output_needs_frame(struct nx_scene_output *scene_output);
// Renders into state, or scans out.
bool nx_scene_output_build_state(struct nx_scene_output *scene_output,
	struct nx_output_state *state);
// Surfaces paced by this output.
void nx_scene_output_send_frame_done(struct nx_scene_output *scene_output,
	struct timespec *now);
// NULL if not in this scene.
struct nx_scene_output *nx_scene_get_scene_output(struct nx_scene *scene,
	struct nx_output *output);

#endif
