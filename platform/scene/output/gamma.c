#include <assert.h>
#include "platform/output/gamma_control.h"
#include "platform/output/output.h"
#include "platform/render/color.h"
#include "platform/scene/output/scene_output.h"

static void scene_handle_gamma_control_manager_v1_set_gamma(struct wl_listener *listener,
		void *data) {
	const struct nx_gamma_control_manager_v1_set_gamma_event *event = data;
	struct nx_scene *scene =
		wl_container_of(listener, scene, gamma_control_manager_v1_set_gamma);
	struct nx_scene_output *output = nx_scene_get_scene_output(scene, event->output);
	if (!output) {
		// Another scene's output.
		return;
	}

	output->gamma_lut_changed = true;
	output->gamma_lut = event->control;
	nx_color_transform_unref(output->gamma_lut_color_transform);
	output->gamma_lut_color_transform = nx_gamma_control_v1_get_color_transform(event->control);
	nx_output_schedule_frame(output->output);
}

static void scene_handle_gamma_control_manager_v1_destroy(struct wl_listener *listener,
		void *data) {
	struct nx_scene *scene =
		wl_container_of(listener, scene, gamma_control_manager_v1_destroy);
	wl_list_remove(&scene->gamma_control_manager_v1_destroy.link);
	wl_list_init(&scene->gamma_control_manager_v1_destroy.link);
	wl_list_remove(&scene->gamma_control_manager_v1_set_gamma.link);
	wl_list_init(&scene->gamma_control_manager_v1_set_gamma.link);
	scene->gamma_control_manager_v1 = NULL;

	struct nx_scene_output *output;
	wl_list_for_each(output, &scene->outputs, link) {
		output->gamma_lut_changed = false;
		output->gamma_lut = NULL;
		nx_color_transform_unref(output->gamma_lut_color_transform);
		output->gamma_lut_color_transform = NULL;
	}
}

void nx_scene_set_gamma_control_manager_v1(struct nx_scene *scene,
		struct nx_gamma_control_manager_v1 *gamma_control) {
	assert(scene->gamma_control_manager_v1 == NULL);
	scene->gamma_control_manager_v1 = gamma_control;

	scene->gamma_control_manager_v1_destroy.notify =
		scene_handle_gamma_control_manager_v1_destroy;
	wl_signal_add(&gamma_control->events.destroy, &scene->gamma_control_manager_v1_destroy);
	scene->gamma_control_manager_v1_set_gamma.notify =
		scene_handle_gamma_control_manager_v1_set_gamma;
	wl_signal_add(&gamma_control->events.set_gamma, &scene->gamma_control_manager_v1_set_gamma);
}

void scene_output_state_attempt_gamma(struct nx_scene_output *scene_output,
		struct nx_output_state *state) {
	if (!scene_output->gamma_lut_changed) {
		return;
	}

	struct nx_output_state gamma_pending = {0};
	if (!nx_output_state_copy(&gamma_pending, state)) {
		return;
	}

	nx_output_state_set_color_transform(&gamma_pending, scene_output->gamma_lut_color_transform);
	scene_output->gamma_lut_changed = false;

	if (!nx_output_test_state(scene_output->output, &gamma_pending)) {
		nx_gamma_control_v1_send_failed_and_destroy(scene_output->gamma_lut);

		scene_output->gamma_lut = NULL;
		nx_color_transform_unref(scene_output->gamma_lut_color_transform);
		scene_output->gamma_lut_color_transform = NULL;
		nx_output_state_finish(&gamma_pending);
		return;
	}

	nx_output_state_copy(state, &gamma_pending);
	nx_output_state_finish(&gamma_pending);
}

// Render-pass LUT; damages on change.
struct nx_color_transform *scene_output_render_gamma(struct nx_scene_output *scene_output) {
	struct nx_output *output = scene_output->output;
	if (nx_output_get_gamma_size(output) != 0 ||
			!output->renderer->features.output_color_transform) {
		return NULL;
	}

	if (scene_output->gamma_lut_color_transform != scene_output->prev_gamma_lut_color_transform) {
		scene_output_damage_whole(scene_output);
	}
	return scene_output->gamma_lut_color_transform;
}
