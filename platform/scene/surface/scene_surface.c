#include <stdlib.h>
#include "platform/output/output.h"
#include "platform/surface/compositor.h"
#include "platform/scene/surface/scene_surface.h"

static void scene_surface_handle_surface_destroy(
		struct wl_listener *listener, void *data) {
	struct nx_scene_surface *surface =
		wl_container_of(listener, surface, surface_destroy);

	nx_scene_node_destroy(&surface->buffer->node);
}

static void scene_surface_handle_surface_commit(
		struct wl_listener *listener, void *data) {
	struct nx_scene_surface *surface =
		wl_container_of(listener, surface, surface_commit);
	struct nx_scene_buffer *scene_buffer = surface->buffer;

	scene_surface_reconfigure(surface);

	// Frame callbacks need a visible output.
	int lx, ly;
	bool enabled = nx_scene_node_coords(&scene_buffer->node, &lx, &ly);
	struct nx_output *output = scene_frame_pacing_output(surface->surface);
	if (!wl_list_empty(&surface->surface->current.frame_callback_list) && output && enabled) {
		nx_output_schedule_frame(output);
	}
}

static int min(int a, int b) {
	return a < b ? a : b;
}

void scene_surface_clipped_size(const struct nx_scene_surface *scene_surface,
		int *width, int *height) {
	const struct nx_surface_state *state = &scene_surface->surface->current;
	*width = state->width;
	*height = state->height;
	if (nx_box_empty(&scene_surface->clip)) {
		return;
	}

	*width = min(scene_surface->clip.width, *width - scene_surface->clip.x);
	*height = min(scene_surface->clip.height, *height - scene_surface->clip.y);
}

// Undo stretching before hit-testing.
static bool scene_buffer_point_accepts_input(struct nx_scene_buffer *scene_buffer,
		double *sx, double *sy) {
	struct nx_scene_surface *scene_surface =
		nx_scene_surface_try_from_buffer(scene_buffer);

	int width, height;
	scene_surface_clipped_size(scene_surface, &width, &height);
	if (scene_buffer->dst_width > 0 && scene_buffer->dst_height > 0 &&
			width > 0 && height > 0) {
		*sx *= (double)width / scene_buffer->dst_width;
		*sy *= (double)height / scene_buffer->dst_height;
	}

	*sx += scene_surface->clip.x;
	*sy += scene_surface->clip.y;

	return nx_surface_point_accepts_input(scene_surface->surface, *sx, *sy);
}

static void surface_addon_destroy(struct nx_addon *addon) {
	struct nx_scene_surface *surface = wl_container_of(addon, surface, addon);

	scene_buffer_unmark_client_buffer(surface->buffer);

	nx_addon_finish(&surface->addon);

	wl_list_remove(&surface->outputs_update.link);
	wl_list_remove(&surface->output_sample.link);
	wl_list_remove(&surface->frame_done.link);
	wl_list_remove(&surface->surface_destroy.link);
	wl_list_remove(&surface->surface_commit.link);

	free(surface);
}

static const struct nx_addon_interface surface_addon_impl = {
	.name = "nx_scene_surface",
	.destroy = surface_addon_destroy,
};

struct nx_scene_surface *nx_scene_surface_try_from_buffer(
		struct nx_scene_buffer *scene_buffer) {
	struct nx_addon *addon = nx_addon_find(&scene_buffer->node.addons,
		scene_buffer, &surface_addon_impl);
	if (!addon) {
		return NULL;
	}

	struct nx_scene_surface *surface = wl_container_of(addon, surface, addon);
	return surface;
}

struct nx_scene_surface *nx_scene_surface_create(struct nx_scene_tree *parent,
		struct nx_surface *nx_surface) {
	struct nx_scene_surface *surface = calloc(1, sizeof(*surface));
	if (surface == NULL) {
		return NULL;
	}

	struct nx_scene_buffer *scene_buffer = nx_scene_buffer_create(parent, NULL);
	if (!scene_buffer) {
		free(surface);
		return NULL;
	}

	surface->buffer = scene_buffer;
	surface->surface = nx_surface;
	scene_buffer->point_accepts_input = scene_buffer_point_accepts_input;

	surface->outputs_update.notify = scene_surface_handle_outputs_update;
	wl_signal_add(&scene_buffer->events.outputs_update, &surface->outputs_update);

	surface->output_sample.notify = scene_surface_handle_output_sample;
	wl_signal_add(&scene_buffer->events.output_sample, &surface->output_sample);

	surface->frame_done.notify = scene_surface_handle_frame_done;
	wl_signal_add(&scene_buffer->events.frame_done, &surface->frame_done);

	surface->surface_destroy.notify = scene_surface_handle_surface_destroy;
	wl_signal_add(&nx_surface->events.destroy, &surface->surface_destroy);

	surface->surface_commit.notify = scene_surface_handle_surface_commit;
	wl_signal_add(&nx_surface->events.commit, &surface->surface_commit);

	nx_addon_init(&surface->addon, &scene_buffer->node.addons,
		scene_buffer, &surface_addon_impl);

	scene_surface_reconfigure(surface);

	return surface;
}

void scene_surface_set_clip(struct nx_scene_surface *surface, const struct nx_box *clip) {
	if (nx_box_equal(clip, &surface->clip)) {
		return;
	}

	surface->clip = clip ? *clip : (struct nx_box){0};
	scene_surface_reconfigure(surface);
}
