#include <assert.h>
#include <drm_fourcc.h>
#include <stdlib.h>
#include "platform/render/swapchain.h"
#include "platform/log/log.h"
#include "platform/os/env.h"
#include "platform/output/output_interface.h"
#include "platform/output/commit/commit.h"

void nx_output_init(struct nx_output *output, struct nx_backend *backend,
		const struct nx_output_impl *impl, struct wl_event_loop *event_loop,
		const struct nx_output_state *state) {
	assert(impl->commit);
	if (impl->set_cursor || impl->move_cursor) {
		assert(impl->set_cursor && impl->move_cursor);
	}

	*output = (struct nx_output){
		.backend = backend,
		.impl = impl,
		.event_loop = event_loop,
		.render_format = DRM_FORMAT_XRGB8888,
		.transform = WL_OUTPUT_TRANSFORM_NORMAL,
		.scale = 1,
	};

	wl_list_init(&output->modes);
	wl_list_init(&output->cursors);
	wl_list_init(&output->resources);

	wl_signal_init(&output->events.frame);
	wl_signal_init(&output->events.damage);
	wl_signal_init(&output->events.needs_frame);
	wl_signal_init(&output->events.precommit);
	wl_signal_init(&output->events.commit);
	wl_signal_init(&output->events.present);
	wl_signal_init(&output->events.bind);
	wl_signal_init(&output->events.description);
	wl_signal_init(&output->events.request_state);
	wl_signal_init(&output->events.destroy);

	output->software_cursor_locks = env_parse_bool("NIXLY_NO_HARDWARE_CURSORS");
	if (output->software_cursor_locks) {
		nx_log(NX_DEBUG, "NIXLY_NO_HARDWARE_CURSORS set, forcing software cursors");
	}

	nx_addon_set_init(&output->addons);
	wl_list_init(&output->display_destroy.link);

	if (state) {
		output_apply_state(output, state);
	}
}

void nx_output_finish(struct nx_output *output) {
	wl_signal_emit_mutable(&output->events.destroy, output);
	nx_addon_set_finish(&output->addons);

	assert(wl_list_empty(&output->events.frame.listener_list));
	assert(wl_list_empty(&output->events.damage.listener_list));
	assert(wl_list_empty(&output->events.needs_frame.listener_list));
	assert(wl_list_empty(&output->events.precommit.listener_list));
	assert(wl_list_empty(&output->events.commit.listener_list));
	assert(wl_list_empty(&output->events.present.listener_list));
	assert(wl_list_empty(&output->events.bind.listener_list));
	assert(wl_list_empty(&output->events.description.listener_list));
	assert(wl_list_empty(&output->events.request_state.listener_list));
	assert(wl_list_empty(&output->events.destroy.listener_list));

	nx_output_destroy_global(output);
	wl_list_remove(&output->display_destroy.link);

	// The backend frees the modes.
	struct nx_output_cursor *cursor, *tmp_cursor;
	wl_list_for_each_safe(cursor, tmp_cursor, &output->cursors, link) {
		nx_output_cursor_destroy(cursor);
	}

	nx_swapchain_destroy(output->cursor_swapchain);
	nx_buffer_unlock(output->cursor_front_buffer);
	nx_color_transform_unref(output->color_transform);

	nx_swapchain_destroy(output->swapchain);

	if (output->idle_frame != NULL) {
		wl_event_source_remove(output->idle_frame);
	}
	if (output->idle_done != NULL) {
		wl_event_source_remove(output->idle_done);
	}

	free(output->name);
	free(output->description);
	free(output->make);
	free(output->model);
	free(output->serial);
}

void nx_output_destroy(struct nx_output *output) {
	if (!output) {
		return;
	}
	if (output->impl && output->impl->destroy) {
		output->impl->destroy(output);
		return;
	}
	nx_output_finish(output);
	free(output);
}

void nx_output_transformed_resolution(struct nx_output *output,
		int *width, int *height) {
	bool rotated = output->transform % 2 != 0;
	*width = rotated ? output->height : output->width;
	*height = rotated ? output->width : output->height;
}

void nx_output_effective_resolution(struct nx_output *output,
		int *width, int *height) {
	nx_output_transformed_resolution(output, width, height);
	*width = (int)((float)*width / output->scale);
	*height = (int)((float)*height / output->scale);
}

struct nx_output_mode *nx_output_preferred_mode(struct nx_output *output) {
	if (wl_list_empty(&output->modes)) {
		return NULL;
	}

	struct nx_output_mode *mode;
	wl_list_for_each(mode, &output->modes, link) {
		if (mode->preferred) {
			return mode;
		}
	}

	// Fall back to the first.
	return wl_container_of(output->modes.next, mode, link);
}

size_t nx_output_get_gamma_size(struct nx_output *output) {
	if (!output->impl->get_gamma_size) {
		return 0;
	}
	return output->impl->get_gamma_size(output);
}
