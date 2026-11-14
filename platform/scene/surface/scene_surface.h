#ifndef PLATFORM_SCENE_SURFACE_SCENE_SURFACE_H
#define PLATFORM_SCENE_SURFACE_SCENE_SURFACE_H

#include "platform/scene/scene.h"

struct nx_image_description_v1_data;

void scene_surface_set_clip(struct nx_scene_surface *surface, const struct nx_box *clip);
void scene_surface_clipped_size(const struct nx_scene_surface *scene_surface,
	int *width, int *height);
void scene_surface_reconfigure(struct nx_scene_surface *scene_surface);
void scene_buffer_unmark_client_buffer(struct nx_scene_buffer *scene_buffer);
struct nx_output *scene_frame_pacing_output(struct nx_surface *surface);
void scene_surface_handle_outputs_update(struct wl_listener *listener, void *data);
void scene_surface_handle_output_sample(struct wl_listener *listener, void *data);
void scene_surface_handle_frame_done(struct wl_listener *listener, void *data);
void scene_preferred_image_description(struct nx_surface *surface,
	struct nx_color_manager_v1 *manager,
	struct nx_image_description_v1_data *out);

#endif
