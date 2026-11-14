#include <assert.h>
#include <string.h>
#include "platform/buffer/linux_dmabuf.h"
#include "platform/output/output.h"
#include "platform/surface/compositor.h"
#include "platform/scene/render/frame.h"

static void scene_handle_linux_dmabuf_v1_destroy(struct wl_listener *listener,
		void *data) {
	struct nx_scene *scene =
		wl_container_of(listener, scene, linux_dmabuf_v1_destroy);
	wl_list_remove(&scene->linux_dmabuf_v1_destroy.link);
	wl_list_init(&scene->linux_dmabuf_v1_destroy.link);
	scene->linux_dmabuf_v1 = NULL;
}

void nx_scene_set_linux_dmabuf_v1(struct nx_scene *scene,
		struct nx_linux_dmabuf_v1 *linux_dmabuf_v1) {
	assert(scene->linux_dmabuf_v1 == NULL);
	scene->linux_dmabuf_v1 = linux_dmabuf_v1;
	scene->linux_dmabuf_v1_destroy.notify = scene_handle_linux_dmabuf_v1_destroy;
	wl_signal_add(&linux_dmabuf_v1->events.destroy, &scene->linux_dmabuf_v1_destroy);
}

static void scene_buffer_send_dmabuf_feedback(const struct nx_scene *scene,
		struct nx_scene_buffer *scene_buffer,
		const struct nx_linux_dmabuf_feedback_v1_init_options *options) {
	if (!scene->linux_dmabuf_v1) {
		return;
	}

	struct nx_scene_surface *surface = nx_scene_surface_try_from_buffer(scene_buffer);
	if (!surface) {
		return;
	}

	// Skip duplicate feedback.
	if (memcmp(options, &scene_buffer->prev_feedback_options, sizeof(*options)) == 0) {
		return;
	}

	scene_buffer->prev_feedback_options = *options;

	struct nx_linux_dmabuf_feedback_v1 feedback = {0};
	if (!nx_linux_dmabuf_feedback_v1_init_with_options(&feedback, options)) {
		return;
	}

	enum wl_output_transform preferred_buffer_transform =
		options->scanout_primary_output != NULL ?
		options->scanout_primary_output->transform : WL_OUTPUT_TRANSFORM_NORMAL;
	nx_surface_set_preferred_buffer_transform(surface->surface, preferred_buffer_transform);
	nx_linux_dmabuf_v1_set_surface_feedback(scene->linux_dmabuf_v1,
		surface->surface, &feedback);

	nx_linux_dmabuf_feedback_v1_finish(&feedback);
}

// Debounced: scanout may flip each frame.
void scene_buffer_send_scanout_feedback(struct nx_scene_output *scene_output,
		struct nx_scene_buffer *scene_buffer) {
	if (scene_output->dmabuf_feedback_debounce < DMABUF_FEEDBACK_DEBOUNCE_FRAMES ||
			scene_buffer->primary_output != scene_output) {
		return;
	}

	struct nx_linux_dmabuf_feedback_v1_init_options options = {
		.main_renderer = scene_output->output->renderer,
		.scanout_primary_output = scene_output->output,
	};
	scene_buffer_send_dmabuf_feedback(scene_output->scene, scene_buffer, &options);
}

void scene_buffer_send_render_feedback(struct nx_scene_output *scene_output,
		struct nx_scene_buffer *scene_buffer) {
	if (scene_output->dmabuf_feedback_debounce != 0 ||
			scene_buffer->primary_output != scene_output) {
		return;
	}

	struct nx_linux_dmabuf_feedback_v1_init_options options = {
		.main_renderer = scene_output->output->renderer,
		.scanout_primary_output = NULL,
	};
	scene_buffer_send_dmabuf_feedback(scene_output->scene, scene_buffer, &options);
}
