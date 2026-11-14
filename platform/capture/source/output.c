#include <assert.h>
#include <stdlib.h>
#include "platform/capture/image_copy/session.h"
#include "platform/capture/source/output_source.h"
#include "platform/capture/source/source.h"
#include "platform/container/addon.h"
#include "platform/output/output.h"
#include "platform/output/output_interface.h"
#include "ext-image-capture-source-v1-protocol.h"

#define OUTPUT_IMAGE_SOURCE_MANAGER_V1_VERSION 1

// One per output.
struct output_source {
	struct capture_source base;
	struct nx_addon addon;

	struct nx_output *output;

	struct wl_listener output_commit;

	struct output_cursor_source cursor;

	size_t num_started;
	bool software_cursors_locked;
};

static void output_source_start(struct capture_source *base,
		bool with_cursors) {
	struct output_source *source = wl_container_of(base, source, base);
	source->num_started++;
	if (source->num_started > 1) {
		return;
	}
	nx_output_lock_attach_render(source->output, true);
	if (with_cursors) {
		nx_output_lock_software_cursors(source->output, true);
	}
	source->software_cursors_locked = with_cursors;
}

static void output_source_stop(struct capture_source *base) {
	struct output_source *source = wl_container_of(base, source, base);
	assert(source->num_started > 0);
	source->num_started--;
	if (source->num_started > 0) {
		return;
	}
	nx_output_lock_attach_render(source->output, false);
	if (source->software_cursors_locked) {
		nx_output_lock_software_cursors(source->output, false);
	}
}

static void output_source_request_frame(struct capture_source *base,
		bool schedule_frame) {
	struct output_source *source = wl_container_of(base, source, base);
	if (schedule_frame) {
		nx_output_update_needs_frame(source->output);
	}
}

static void output_source_copy_frame(struct capture_source *base,
		struct image_copy_frame *frame,
		struct capture_source_frame_event *base_event) {
	struct output_source *source = wl_container_of(base, source, base);
	struct output_frame_event *event = wl_container_of(base_event, event, base);

	if (image_copy_frame_copy_buffer(frame, event->buffer,
			source->output->renderer)) {
		image_copy_frame_ready(frame, source->output->transform, &event->when);
	}
}

static struct capture_cursor_source *output_source_get_pointer_cursor(
		struct capture_source *base, struct nx_seat *seat) {
	struct output_source *source = wl_container_of(base, source, base);
	return &source->cursor.base;
}

static const struct capture_source_impl output_source_impl = {
	.start = output_source_start,
	.stop = output_source_stop,
	.request_frame = output_source_request_frame,
	.copy_frame = output_source_copy_frame,
	.get_pointer_cursor = output_source_get_pointer_cursor,
};

static void source_update_buffer_constraints(struct output_source *source) {
	struct nx_output *output = source->output;

	if (!output->enabled) {
		return;
	}

	if (!nx_output_configure_primary_swapchain(output, NULL, &output->swapchain)) {
		return;
	}

	capture_source_set_constraints_from_swapchain(&source->base,
		output->swapchain, output->renderer);
}

static void source_send_frame(struct output_source *source,
		const struct nx_output_event_commit *event) {
	struct nx_buffer *buffer = event->state->buffer;

	pixman_region32_t full_damage;
	pixman_region32_init_rect(&full_damage, 0, 0, buffer->width, buffer->height);

	const pixman_region32_t *damage = &full_damage;
	if (event->state->committed & NX_OUTPUT_STATE_DAMAGE) {
		damage = &event->state->damage;
	}

	struct output_frame_event frame_event = {
		.base = {
			.damage = damage,
		},
		.buffer = buffer,
		.when = event->when,
	};
	wl_signal_emit_mutable(&source->base.events.frame, &frame_event);

	pixman_region32_fini(&full_damage);
}

static void source_handle_output_commit(struct wl_listener *listener,
		void *data) {
	struct output_source *source =
		wl_container_of(listener, source, output_commit);
	struct nx_output_event_commit *event = data;

	if (event->state->committed & (NX_OUTPUT_STATE_MODE |
			NX_OUTPUT_STATE_RENDER_FORMAT | NX_OUTPUT_STATE_ENABLED)) {
		source_update_buffer_constraints(source);
	}

	if (event->state->committed & NX_OUTPUT_STATE_BUFFER) {
		source_send_frame(source, event);
	}
}

static void output_addon_destroy(struct nx_addon *addon) {
	struct output_source *source = wl_container_of(addon, source, addon);
	capture_source_finish(&source->base);
	capture_output_cursor_finish(&source->cursor);
	wl_list_remove(&source->output_commit.link);
	nx_addon_finish(&source->addon);
	free(source);
}

static const struct nx_addon_interface output_addon_impl = {
	.name = "nx_ext_output_image_capture_source_v1",
	.destroy = output_addon_destroy,
};

static struct output_source *output_source_get_or_create(
		struct nx_output *output) {
	struct nx_addon *addon =
		nx_addon_find(&output->addons, NULL, &output_addon_impl);
	if (addon != NULL) {
		struct output_source *source = wl_container_of(addon, source, addon);
		return source;
	}

	struct output_source *source = calloc(1, sizeof(*source));
	if (source == NULL) {
		return NULL;
	}

	capture_source_init(&source->base, &output_source_impl);
	nx_addon_init(&source->addon, &output->addons, NULL, &output_addon_impl);
	source->output = output;

	source->output_commit.notify = source_handle_output_commit;
	wl_signal_add(&output->events.commit, &source->output_commit);

	source_update_buffer_constraints(source);

	capture_output_cursor_init(&source->cursor, output);
	return source;
}

static void manager_handle_create_source(struct wl_client *client,
		struct wl_resource *manager_resource, uint32_t new_id,
		struct wl_resource *output_resource) {
	struct nx_output *output = nx_output_from_resource(output_resource);
	if (output == NULL) {
		capture_source_create_resource(NULL, client, new_id);
		return;
	}

	struct output_source *source = output_source_get_or_create(output);
	if (source == NULL) {
		wl_resource_post_no_memory(manager_resource);
		return;
	}

	capture_source_create_resource(&source->base, client, new_id);
}

static void manager_handle_destroy(struct wl_client *client,
		struct wl_resource *manager_resource) {
	wl_resource_destroy(manager_resource);
}

static const struct ext_output_image_capture_source_manager_v1_interface
		manager_impl = {
	.create_source = manager_handle_create_source,
	.destroy = manager_handle_destroy,
};

static void manager_bind(struct wl_client *client, void *data,
		uint32_t version, uint32_t id) {
	struct wl_resource *resource = wl_resource_create(client,
		&ext_output_image_capture_source_manager_v1_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &manager_impl, data, NULL);
}

static void manager_handle_display_destroy(struct wl_listener *listener,
		void *data) {
	struct nx_ext_output_image_capture_source_manager_v1 *manager =
		wl_container_of(listener, manager, display_destroy);
	wl_list_remove(&manager->display_destroy.link);
	wl_global_destroy(manager->global);
	free(manager);
}

struct nx_ext_output_image_capture_source_manager_v1 *
		nx_ext_output_image_capture_source_manager_v1_create(
		struct wl_display *display, uint32_t version) {
	assert(version <= OUTPUT_IMAGE_SOURCE_MANAGER_V1_VERSION);

	struct nx_ext_output_image_capture_source_manager_v1 *manager =
		calloc(1, sizeof(*manager));
	if (manager == NULL) {
		return NULL;
	}

	manager->global = wl_global_create(display,
		&ext_output_image_capture_source_manager_v1_interface, version,
		manager, manager_bind);
	if (manager->global == NULL) {
		free(manager);
		return NULL;
	}

	manager->display_destroy.notify = manager_handle_display_destroy;
	wl_display_add_destroy_listener(display, &manager->display_destroy);

	return manager;
}
