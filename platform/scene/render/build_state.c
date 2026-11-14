#include <assert.h>
#include <math.h>
#include "platform/geometry/region.h"
#include "platform/geometry/transform.h"
#include "platform/log/log.h"
#include "platform/output/damage_ring.h"
#include "platform/output/output.h"
#include "platform/output/commit/commit.h"
#include "platform/render/swapchain.h"
#include "platform/scene/graph/node.h"
#include "platform/scene/output/scene_output.h"
#include "platform/scene/render/frame.h"

bool nx_scene_output_needs_frame(struct nx_scene_output *scene_output) {
	return scene_output->output->needs_frame ||
		!pixman_region32_empty(&scene_output->pending_commit_damage) ||
		scene_output->gamma_lut_changed;
}

// Geometry changes damage everything.
static void render_data_init(struct render_data *data,
		struct nx_scene_output *scene_output, const struct nx_output_state *state) {
	struct nx_output *output = scene_output->output;
	*data = (struct render_data){
		.transform = output->transform,
		.scale = output->scale,
		.output = scene_output,
	};
	output_pending_resolution(output, state, &data->width, &data->height);

	if ((state->committed & NX_OUTPUT_STATE_TRANSFORM) &&
			data->transform != state->transform) {
		scene_output_damage_whole(scene_output);
		data->transform = state->transform;
	}

	if ((state->committed & NX_OUTPUT_STATE_SCALE) && data->scale != state->scale) {
		scene_output_damage_whole(scene_output);
		data->scale = state->scale;
	}

	data->trans_width = data->width;
	data->trans_height = data->height;
	nx_output_transform_coords(data->transform, &data->trans_width, &data->trans_height);

	data->logical = (struct nx_box){
		.x = scene_output->x,
		.y = scene_output->y,
		.width = (int)(data->trans_width / data->scale),
		.height = (int)(data->trans_height / data->scale),
	};
}

// Debounces feedback and logs changes.
static bool scene_output_track_scanout(struct nx_scene_output *scene_output,
		enum scene_direct_scanout_result result) {
	if (result == SCANOUT_INELIGIBLE && scene_output->dmabuf_feedback_debounce > 0) {
		scene_output->dmabuf_feedback_debounce--;
	} else if (result != SCANOUT_INELIGIBLE &&
			scene_output->dmabuf_feedback_debounce < DMABUF_FEEDBACK_DEBOUNCE_FRAMES) {
		scene_output->dmabuf_feedback_debounce++;
	}

	bool scanout = result == SCANOUT_SUCCESS;
	if (scene_output->prev_scanout != scanout) {
		scene_output->prev_scanout = scanout;
		nx_log(NX_DEBUG, "Direct scan-out %s",
			scanout ? "enabled" : "disabled");
	}
	return scanout;
}

static bool scene_output_try_scanout(const struct render_data *data,
		struct nx_output_state *state, const struct nx_color_transform *render_gamma) {
	struct nx_scene_output *scene_output = data->output;
	size_t list_len = scene_output->render_list.size / sizeof(struct render_list_entry);

	// Single entry, no LUT or highlight.
	enum scene_direct_scanout_result result = SCANOUT_INELIGIBLE;
	if (render_gamma == NULL && list_len == 1 &&
			scene_output->scene->debug_damage_option != NX_SCENE_DEBUG_DAMAGE_HIGHLIGHT) {
		result = scene_entry_try_direct_scanout(scene_output->render_list.data, state, data);
	}

	return scene_output_track_scanout(scene_output, result);
}

static bool scene_output_update_color_transform(struct nx_scene_output *scene_output,
		const struct nx_output_state *state, struct nx_color_transform *render_gamma) {
	if ((render_gamma == NULL ||
			render_gamma == scene_output->prev_gamma_lut_color_transform) &&
			!(state->committed & NX_OUTPUT_STATE_IMAGE_DESCRIPTION)) {
		return true;
	}

	const struct nx_output_image_description *output_description =
		output_pending_image_description(scene_output->output, state);
	return scene_output_combine_color_transforms(scene_output,
		output_description, render_gamma);
}

// Opaque nodes hide the background.
static void background_cull_opaque(pixman_region32_t *background,
		const struct render_data *data) {
	struct nx_scene_output *scene_output = data->output;
	const struct render_list_entry *list_data = scene_output->render_list.data;
	int list_len = scene_output->render_list.size / sizeof(*list_data);
	for (int i = list_len - 1; i >= 0; i--) {
		const struct render_list_entry *entry = &list_data[i];
		if (entry->opacity != 1) {
			continue;
		}

		pixman_region32_t opaque;
		pixman_region32_init(&opaque);
		scene_node_opaque_region(entry->node, entry->x, entry->y, &opaque);
		// Culled black rects need background.
		pixman_region32_intersect(&opaque, &opaque, &entry->node->visible);

		pixman_region32_translate(&opaque, -scene_output->x, -scene_output->y);
		scene_logical_to_buffer(&opaque, data);
		pixman_region32_subtract(background, background, &opaque);
		pixman_region32_fini(&opaque);
	}

	if (floor(data->scale) == data->scale) {
		return;
	}

	// Expanded, but never past the damage.
	nx_region_expand(background, background, 1);
	pixman_region32_intersect(background, background, &data->damage);
}

static void render_background(const struct render_data *data,
		const struct nx_buffer *buffer) {
	pixman_region32_t background;
	pixman_region32_init(&background);
	pixman_region32_copy(&background, &data->damage);
	if (data->output->scene->calculate_visibility) {
		background_cull_opaque(&background, data);
	}

	nx_render_pass_add_rect(data->render_pass, &(struct nx_render_rect_options){
		.box = { .width = buffer->width, .height = buffer->height },
		.color = { .r = 0, .g = 0, .b = 0, .a = 1 },
		.clip = &background,
	});
	pixman_region32_fini(&background);
}

static void scene_output_draw(struct render_data *data, struct nx_buffer *buffer) {
	struct nx_scene_output *scene_output = data->output;
	pixman_region32_init(&data->damage);
	nx_damage_ring_rotate_buffer(&scene_output->damage_ring, buffer, &data->damage);

	render_background(data, buffer);

	struct render_list_entry *list_data = scene_output->render_list.data;
	int list_len = scene_output->render_list.size / sizeof(*list_data);
	for (int i = list_len - 1; i >= 0; i--) {
		struct render_list_entry *entry = &list_data[i];
		scene_entry_render(entry, data);
		if (entry->node->type == NX_SCENE_NODE_BUFFER) {
			scene_buffer_send_render_feedback(scene_output,
				nx_scene_buffer_from_node(entry->node));
		}
	}

	scene_debug_damage_draw(data, buffer);

	nx_output_add_software_cursors_to_render_pass(scene_output->output,
		data->render_pass, &data->damage);
	pixman_region32_fini(&data->damage);
}

static bool scene_output_composite(struct render_data *data,
		struct nx_output_state *state, struct nx_color_transform *render_gamma) {
	struct nx_scene_output *scene_output = data->output;
	struct nx_output *output = scene_output->output;
	if (!nx_output_configure_primary_swapchain(output, state, &output->swapchain)) {
		return false;
	}

	struct nx_buffer *buffer = nx_swapchain_acquire(output->swapchain);
	if (buffer == NULL) {
		return false;
	}

	assert(buffer->width == data->width && buffer->height == data->height);

	if (!scene_output_update_color_transform(scene_output, state, render_gamma)) {
		nx_buffer_unlock(buffer);
		return false;
	}

	scene_output->in_point++;
	data->render_pass = nx_renderer_begin_buffer_pass(output->renderer, buffer,
			&(struct nx_buffer_pass_options){
		.color_transform = scene_output->combined_color_transform,
		.signal_timeline = scene_output->in_timeline,
		.signal_point = scene_output->in_point,
	});
	if (data->render_pass == NULL) {
		nx_buffer_unlock(buffer);
		return false;
	}

	scene_output_draw(data, buffer);

	if (!nx_render_pass_submit(data->render_pass)) {
		nx_buffer_unlock(buffer);
		// Failed renders leave undefined contents.
		nx_damage_ring_add_whole(&scene_output->damage_ring);
		return false;
	}

	nx_output_state_set_buffer(state, buffer);
	nx_buffer_unlock(buffer);
	return true;
}

bool nx_scene_output_build_state(struct nx_scene_output *scene_output,
		struct nx_output_state *state) {
	// Disabling needs no frame.
	if ((state->committed & NX_OUTPUT_STATE_ENABLED) && !state->enabled) {
		return true;
	}

	struct nx_color_transform *render_gamma = scene_output_render_gamma(scene_output);

	struct render_data render_data;
	render_data_init(&render_data, scene_output, state);
	scene_output_build_render_list(scene_output, &render_data);
	scene_debug_damage_prepare(scene_output, &render_data.now);

	nx_output_state_set_damage(state, &scene_output->pending_commit_damage);

	if (scene_output_try_scanout(&render_data, state, render_gamma)) {
		scene_output_state_attempt_gamma(scene_output, state);
		return true;
	}

	if (!scene_output_composite(&render_data, state, render_gamma)) {
		return false;
	}

	if (scene_output->in_timeline != NULL) {
		nx_output_state_set_wait_timeline(state, scene_output->in_timeline,
			scene_output->in_point);
		scene_output->out_point++;
		nx_output_state_set_signal_timeline(state, scene_output->out_timeline,
			scene_output->out_point);
	}

	if (render_gamma == NULL) {
		scene_output_state_attempt_gamma(scene_output, state);
	}

	return true;
}
