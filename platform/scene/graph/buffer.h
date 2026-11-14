#ifndef PLATFORM_SCENE_GRAPH_BUFFER_H
#define PLATFORM_SCENE_GRAPH_BUFFER_H

#include "platform/scene/scene.h"

void scene_buffer_set_buffer(struct nx_scene_buffer *scene_buffer,
	struct nx_buffer *buffer);
void scene_buffer_set_texture(struct nx_scene_buffer *scene_buffer,
	struct nx_texture *texture);
void scene_buffer_set_wait_timeline(struct nx_scene_buffer *scene_buffer,
	struct nx_drm_syncobj_timeline *timeline, uint64_t point);
struct nx_texture *scene_buffer_get_texture(
	struct nx_scene_buffer *scene_buffer, struct nx_renderer *renderer);

#endif
