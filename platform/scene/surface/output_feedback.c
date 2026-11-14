#include <math.h>
#include "platform/output/output.h"
#include "platform/output/presentation.h"
#include "platform/render/drm_syncobj_merger.h"
#include "platform/surface/color_management.h"
#include "platform/surface/compositor.h"
#include "platform/surface/fifo.h"
#include "platform/surface/fractional_scale.h"
#include "platform/surface/syncobj.h"
#include "platform/scene/graph/node.h"
#include "platform/scene/surface/scene_surface.h"

static double preferred_buffer_scale(struct nx_surface *surface) {
	double scale = 1;
	struct nx_surface_output *surface_output;
	wl_list_for_each(surface_output, &surface->current_outputs, link) {
		if (surface_output->output->scale > scale) {
			scale = surface_output->output->scale;
		}
	}
	return scale;
}

// Fastest unsuspended output, or NULL.
struct nx_output *scene_frame_pacing_output(struct nx_surface *surface) {
	struct nx_output *frame_pacing_output = NULL;
	struct nx_surface_output *surface_output;
	wl_list_for_each(surface_output, &surface->current_outputs, link) {
		if (!surface_output->suspended && (frame_pacing_output == NULL ||
				surface_output->output->refresh > frame_pacing_output->refresh)) {
			frame_pacing_output = surface_output->output;
		}
	}
	return frame_pacing_output;
}

static bool output_is_active(const struct nx_scene_outputs_update_event *event,
		const struct nx_output *output) {
	for (size_t i = 0; i < event->size; i++) {
		if (output == event->active[i]->output) {
			return true;
		}
	}
	return false;
}

static bool scene_has_output(struct nx_scene *scene, const struct nx_output *output) {
	struct nx_scene_output *scene_output;
	wl_list_for_each(scene_output, &scene->outputs, link) {
		if (scene_output->output == output) {
			return true;
		}
	}
	return false;
}

// Shown again: leave stale outputs.
static void scene_surface_update_entered(struct nx_scene_surface *surface,
		struct nx_scene *scene, const struct nx_scene_outputs_update_event *event) {
	bool suspend = event->size == 0;
	struct nx_surface_output *entered_output, *tmp;
	wl_list_for_each_safe(entered_output, tmp, &surface->surface->current_outputs, link) {
		if (!scene_has_output(scene, entered_output->output)) {
			continue;
		}

		entered_output->suspended = suspend;
		if (!suspend && !output_is_active(event, entered_output->output)) {
			nx_surface_send_leave(surface->surface, entered_output->output);
		}
	}
}

// Hidden: unblock fence-waiting clients.
static void scene_surface_drop_release_merger(struct nx_surface *surface) {
	struct nx_linux_drm_syncobj_surface_v1_state *syncobj_state =
		nx_linux_drm_syncobj_v1_get_surface_state(surface);
	if (syncobj_state == NULL || syncobj_state->release_merger == NULL) {
		return;
	}

	nx_drm_syncobj_merger_unref(syncobj_state->release_merger);
	syncobj_state->release_merger = NULL;
}

static void scene_surface_send_preferences(struct nx_scene_surface *surface,
		struct nx_scene *scene) {
	double scale = preferred_buffer_scale(surface->surface);
	nx_fractional_scale_v1_notify_scale(surface->surface, scale);
	nx_surface_set_preferred_buffer_scale(surface->surface, (int32_t)ceil(scale));

	if (scene->color_manager_v1 == NULL) {
		return;
	}

	struct nx_image_description_v1_data img_desc = {0};
	scene_preferred_image_description(surface->surface,
		scene->color_manager_v1, &img_desc);
	nx_color_manager_v1_set_surface_preferred_image_description(scene->color_manager_v1,
		surface->surface, &img_desc);
}

void scene_surface_handle_outputs_update(
		struct wl_listener *listener, void *data) {
	struct nx_scene_surface *surface =
		wl_container_of(listener, surface, outputs_update);
	struct nx_scene_outputs_update_event *event = data;
	struct nx_scene *scene = scene_node_get_root(&surface->buffer->node);

	scene_surface_update_entered(surface, scene, event);

	// Hidden surfaces keep their preferences.
	if (event->size == 0) {
		scene_surface_drop_release_merger(surface->surface);
		return;
	}

	// Disabling stripped the buffer.
	if (surface->buffer->buffer == NULL && surface->surface->buffer != NULL) {
		scene_surface_reconfigure(surface);
	}

	// Repeated enters are filtered.
	for (size_t i = 0; i < event->size; i++) {
		nx_surface_send_enter(surface->surface, event->active[i]->output);
	}

	scene_surface_send_preferences(surface, scene);
}

void scene_surface_handle_output_sample(
		struct wl_listener *listener, void *data) {
	struct nx_scene_surface *surface =
		wl_container_of(listener, surface, output_sample);
	const struct nx_scene_output_sample_event *event = data;
	struct nx_output *output = event->output->output;

	struct nx_linux_drm_syncobj_surface_v1_state *syncobj_surface_state =
		nx_linux_drm_syncobj_v1_get_surface_state(surface->surface);
	if (syncobj_surface_state != NULL && event->release_timeline != NULL) {
		nx_linux_drm_syncobj_v1_state_add_release_point(syncobj_surface_state,
			event->release_timeline, event->release_point, output->event_loop);
	}

	// Latched: release waiting fifo commits.
	nx_fifo_v1_surface_latched(surface->surface);

	if (scene_frame_pacing_output(surface->surface) != output) {
		return;
	}

	if (event->direct_scanout) {
		nx_presentation_surface_scanned_out_on_output(surface->surface, output);
		return;
	}

	nx_presentation_surface_textured_on_output(surface->surface, output);
}

void scene_surface_handle_frame_done(
		struct wl_listener *listener, void *data) {
	struct nx_scene_surface *surface =
		wl_container_of(listener, surface, frame_done);
	struct nx_scene_frame_done_event *event = data;
	if (scene_frame_pacing_output(surface->surface) != event->output->output) {
		return;
	}

	nx_surface_send_frame_done(surface->surface, &event->when);
}
