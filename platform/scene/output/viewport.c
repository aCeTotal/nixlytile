#include <assert.h>
#include <stdlib.h>
#include "platform/backend/backend.h"
#include "platform/geometry/region.h"
#include "platform/geometry/transform.h"
#include "platform/output/damage_ring.h"
#include "platform/output/output.h"
#include "platform/render/color.h"
#include "platform/render/drm_syncobj.h"
#include "platform/scene/graph/node.h"
#include "platform/scene/output/scene_output.h"
#include "platform/scene/render/frame.h"

#define FORCED_UPDATE_STATE (NX_OUTPUT_STATE_TRANSFORM | \
	NX_OUTPUT_STATE_SCALE | NX_OUTPUT_STATE_SUBPIXEL)

static void scene_output_handle_destroy(struct nx_addon *addon) {
	struct nx_scene_output *scene_output =
		wl_container_of(addon, scene_output, addon);
	nx_scene_output_destroy(scene_output);
}

static const struct nx_addon_interface output_addon_impl = {
	.name = "nx_scene_output",
	.destroy = scene_output_handle_destroy,
};

// Buffers on force always update.
static void scene_output_update_geometry(struct nx_scene_output *scene_output,
		struct nx_scene_output *force) {
	scene_output_damage_whole(scene_output);

	scene_subtree_update_outputs(&scene_output->scene->tree.node,
		&scene_output->scene->outputs, NULL, force);
}

// Committed damage needs no tracking.
static void scene_output_forget_committed_damage(struct nx_scene_output *scene_output,
		const struct nx_output_state *state) {
	if (!(state->committed & NX_OUTPUT_STATE_BUFFER)) {
		return;
	}

	if (state->committed & NX_OUTPUT_STATE_DAMAGE) {
		pixman_region32_subtract(&scene_output->pending_commit_damage,
			&scene_output->pending_commit_damage, &state->damage);
		return;
	}

	pixman_region32_fini(&scene_output->pending_commit_damage);
	pixman_region32_init(&scene_output->pending_commit_damage);
}

static void scene_output_handle_commit(struct wl_listener *listener, void *data) {
	struct nx_scene_output *scene_output = wl_container_of(listener,
		scene_output, output_commit);
	struct nx_output_event_commit *event = data;
	const struct nx_output_state *state = event->state;

	scene_output_forget_committed_damage(scene_output, state);

	if (state->committed & (FORCED_UPDATE_STATE | NX_OUTPUT_STATE_MODE |
			NX_OUTPUT_STATE_ENABLED)) {
		scene_output_update_geometry(scene_output,
			state->committed & FORCED_UPDATE_STATE ? scene_output : NULL);
	}

	if (scene_output->scene->debug_damage_option == NX_SCENE_DEBUG_DAMAGE_HIGHLIGHT &&
			!wl_list_empty(&scene_output->damage_highlight_regions)) {
		nx_output_schedule_frame(scene_output->output);
	}

	// Retry the gamma LUT once enabled.
	if (scene_output->scene->gamma_control_manager_v1 &&
			(state->committed & NX_OUTPUT_STATE_ENABLED) &&
			!scene_output->output->enabled) {
		scene_output->gamma_lut_changed = true;
	}
}

static void scene_output_handle_damage(struct wl_listener *listener, void *data) {
	struct nx_scene_output *scene_output = wl_container_of(listener,
		scene_output, output_damage);
	struct nx_output *output = scene_output->output;
	struct nx_output_event_damage *event = data;

	int width, height;
	nx_output_transformed_resolution(output, &width, &height);

	pixman_region32_t damage;
	pixman_region32_init(&damage);
	pixman_region32_copy(&damage, event->damage);
	nx_region_transform(&damage, &damage,
		nx_output_transform_invert(output->transform), width, height);
	scene_output_damage(scene_output, &damage);
	pixman_region32_fini(&damage);
}

static void scene_output_handle_needs_frame(struct wl_listener *listener, void *data) {
	struct nx_scene_output *scene_output = wl_container_of(listener,
		scene_output, output_needs_frame);
	nx_output_schedule_frame(scene_output->output);
}

static bool scene_output_init_timelines(struct nx_scene_output *scene_output) {
	struct nx_output *output = scene_output->output;
	int drm_fd = nx_backend_get_drm_fd(output->backend);
	if (drm_fd < 0 || !output->backend->features.timeline ||
			output->renderer == NULL || !output->renderer->features.timeline) {
		return true;
	}

	scene_output->in_timeline = nx_drm_syncobj_timeline_create(drm_fd);
	scene_output->out_timeline = nx_drm_syncobj_timeline_create(drm_fd);
	if (scene_output->in_timeline != NULL && scene_output->out_timeline != NULL) {
		return true;
	}

	nx_drm_syncobj_timeline_unref(scene_output->in_timeline);
	nx_drm_syncobj_timeline_unref(scene_output->out_timeline);
	return false;
}

struct nx_scene_output *nx_scene_output_create(struct nx_scene *scene,
		struct nx_output *output) {
	struct nx_scene_output *scene_output = calloc(1, sizeof(*scene_output));
	if (scene_output == NULL) {
		return NULL;
	}

	scene_output->output = output;
	scene_output->scene = scene;
	nx_addon_init(&scene_output->addon, &output->addons, scene, &output_addon_impl);

	nx_damage_ring_init(&scene_output->damage_ring);
	pixman_region32_init(&scene_output->pending_commit_damage);
	wl_list_init(&scene_output->damage_highlight_regions);

	// Take the lowest free index.
	int prev_output_index = -1;
	struct wl_list *prev_output_link = &scene->outputs;
	struct nx_scene_output *current_output;
	wl_list_for_each(current_output, &scene->outputs, link) {
		if (prev_output_index + 1 != current_output->index) {
			break;
		}

		prev_output_index = current_output->index;
		prev_output_link = &current_output->link;
	}

	if (!scene_output_init_timelines(scene_output)) {
		return NULL;
	}

	scene_output->index = prev_output_index + 1;
	assert(scene_output->index < SCENE_OUTPUT_MAX);
	wl_list_insert(prev_output_link, &scene_output->link);

	scene_output->output_commit.notify = scene_output_handle_commit;
	wl_signal_add(&output->events.commit, &scene_output->output_commit);

	scene_output->output_damage.notify = scene_output_handle_damage;
	wl_signal_add(&output->events.damage, &scene_output->output_damage);

	scene_output->output_needs_frame.notify = scene_output_handle_needs_frame;
	wl_signal_add(&output->events.needs_frame, &scene_output->output_needs_frame);

	scene_output_update_geometry(scene_output, NULL);

	return scene_output;
}

static void timeline_finish(struct nx_drm_syncobj_timeline *timeline) {
	if (timeline == NULL) {
		return;
	}

	nx_drm_syncobj_timeline_signal(timeline, UINT64_MAX);
	nx_drm_syncobj_timeline_unref(timeline);
}

void nx_scene_output_destroy(struct nx_scene_output *scene_output) {
	if (scene_output == NULL) {
		return;
	}

	scene_subtree_update_outputs(&scene_output->scene->tree.node,
		&scene_output->scene->outputs, scene_output, NULL);

	scene_debug_damage_finish(scene_output);

	nx_addon_finish(&scene_output->addon);
	nx_damage_ring_finish(&scene_output->damage_ring);
	pixman_region32_fini(&scene_output->pending_commit_damage);
	wl_list_remove(&scene_output->link);
	wl_list_remove(&scene_output->output_commit.link);
	wl_list_remove(&scene_output->output_damage.link);
	wl_list_remove(&scene_output->output_needs_frame.link);
	timeline_finish(scene_output->in_timeline);
	timeline_finish(scene_output->out_timeline);
	nx_color_transform_unref(scene_output->gamma_lut_color_transform);
	nx_color_transform_unref(scene_output->prev_gamma_lut_color_transform);
	nx_color_transform_unref(scene_output->combined_color_transform);
	wl_array_release(&scene_output->render_list);
	free(scene_output);
}

struct nx_scene_output *nx_scene_get_scene_output(struct nx_scene *scene,
		struct nx_output *output) {
	struct nx_addon *addon =
		nx_addon_find(&output->addons, scene, &output_addon_impl);
	if (addon == NULL) {
		return NULL;
	}
	struct nx_scene_output *scene_output =
		wl_container_of(addon, scene_output, addon);
	return scene_output;
}

void nx_scene_output_set_position(struct nx_scene_output *scene_output,
		int lx, int ly) {
	if (scene_output->x == lx && scene_output->y == ly) {
		return;
	}

	scene_output->x = lx;
	scene_output->y = ly;

	scene_output_update_geometry(scene_output, NULL);
}
