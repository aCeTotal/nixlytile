#include <assert.h>
#include <stdlib.h>
#include "platform/input/keyboard/text_input.h"
#include "platform/log/log.h"
#include "platform/surface/compositor.h"
#include "text-input-unstable-v3-protocol.h"

static void text_input_clear_focused_surface(struct nx_text_input_v3 *text_input) {
	wl_list_remove(&text_input->surface_destroy.link);
	wl_list_init(&text_input->surface_destroy.link);
	text_input->focused_surface = NULL;
}

static const struct zwp_text_input_v3_interface text_input_impl;

static struct nx_text_input_v3 *text_input_from_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &zwp_text_input_v3_interface,
		&text_input_impl));
	return wl_resource_get_user_data(resource);
}

void nx_text_input_v3_send_enter(struct nx_text_input_v3 *text_input,
		struct nx_surface *surface) {
	assert(wl_resource_get_client(text_input->resource)
		== wl_resource_get_client(surface->resource));
	assert(text_input->focused_surface == NULL);
	text_input->focused_surface = surface;
	wl_signal_add(&text_input->focused_surface->events.destroy,
		&text_input->surface_destroy);
	zwp_text_input_v3_send_enter(text_input->resource,
		text_input->focused_surface->resource);
}

void nx_text_input_v3_send_leave(struct nx_text_input_v3 *text_input) {
	assert(text_input->focused_surface != NULL);
	zwp_text_input_v3_send_leave(text_input->resource,
		text_input->focused_surface->resource);
	text_input_clear_focused_surface(text_input);
}

static void text_input_destroy(struct nx_text_input_v3 *text_input) {
	wl_signal_emit_mutable(&text_input->events.destroy, NULL);

	assert(wl_list_empty(&text_input->events.enable.listener_list));
	assert(wl_list_empty(&text_input->events.commit.listener_list));
	assert(wl_list_empty(&text_input->events.disable.listener_list));
	assert(wl_list_empty(&text_input->events.destroy.listener_list));

	text_input_clear_focused_surface(text_input);
	wl_list_remove(&text_input->seat_destroy.link);
	free(text_input);
}

static void text_input_resource_destroy(struct wl_resource *resource) {
	struct nx_text_input_v3 *text_input = text_input_from_resource(resource);
	if (!text_input) {
		return;
	}
	text_input_destroy(text_input);
}

static void text_input_handle_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static void text_input_set_enabled(struct wl_resource *resource, bool enabled) {
	struct nx_text_input_v3 *text_input = text_input_from_resource(resource);
	if (!text_input) {
		return;
	}
	text_input->pending_enabled = enabled;
}

static void text_input_enable(struct wl_client *client,
		struct wl_resource *resource) {
	text_input_set_enabled(resource, true);
}

static void text_input_disable(struct wl_client *client,
		struct wl_resource *resource) {
	text_input_set_enabled(resource, false);
}

// No input method uses state.
static void text_input_set_surrounding_text(struct wl_client *client,
		struct wl_resource *resource, const char *text, int32_t cursor,
		int32_t anchor) {
}

static void text_input_set_text_change_cause(struct wl_client *client,
		struct wl_resource *resource, uint32_t cause) {
}

static void text_input_set_content_type(struct wl_client *client,
		struct wl_resource *resource, uint32_t hint, uint32_t purpose) {
}

static void text_input_set_cursor_rectangle(struct wl_client *client,
		struct wl_resource *resource, int32_t x, int32_t y, int32_t width,
		int32_t height) {
}

static void text_input_commit(struct wl_client *client,
		struct wl_resource *resource) {
	struct nx_text_input_v3 *text_input = text_input_from_resource(resource);
	if (!text_input) {
		return;
	}

	bool old_enabled = text_input->current_enabled;
	text_input->current_enabled = text_input->pending_enabled;

	if (text_input->focused_surface == NULL) {
		nx_log(NX_DEBUG, "Text input commit received without focus");
	}

	if (!old_enabled && text_input->current_enabled) {
		wl_signal_emit_mutable(&text_input->events.enable, NULL);
		return;
	}
	if (old_enabled && !text_input->current_enabled) {
		wl_signal_emit_mutable(&text_input->events.disable, NULL);
		return;
	}
	wl_signal_emit_mutable(&text_input->events.commit, NULL);
}

static const struct zwp_text_input_v3_interface text_input_impl = {
	.destroy = text_input_handle_destroy,
	.enable = text_input_enable,
	.disable = text_input_disable,
	.set_surrounding_text = text_input_set_surrounding_text,
	.set_text_change_cause = text_input_set_text_change_cause,
	.set_content_type = text_input_set_content_type,
	.set_cursor_rectangle = text_input_set_cursor_rectangle,
	.commit = text_input_commit,
};

static void text_input_manager_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static void text_input_handle_seat_destroy(struct wl_listener *listener,
		void *data) {
	struct nx_text_input_v3 *text_input = wl_container_of(listener, text_input,
		seat_destroy);
	struct wl_resource *resource = text_input->resource;
	text_input_destroy(text_input);
	wl_resource_set_user_data(resource, NULL);
}

static void text_input_handle_focused_surface_destroy(
		struct wl_listener *listener, void *data) {
	struct nx_text_input_v3 *text_input = wl_container_of(listener, text_input,
		surface_destroy);
	text_input_clear_focused_surface(text_input);
}

static const struct zwp_text_input_manager_v3_interface text_input_manager_impl;

static struct nx_text_input_manager_v3 *text_input_manager_from_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource,
		&zwp_text_input_manager_v3_interface, &text_input_manager_impl));
	return wl_resource_get_user_data(resource);
}

static void text_input_manager_get_text_input(struct wl_client *client,
		struct wl_resource *resource, uint32_t id, struct wl_resource *seat) {
	int version = wl_resource_get_version(resource);
	struct wl_resource *text_input_resource = wl_resource_create(client,
		&zwp_text_input_v3_interface, version, id);
	if (text_input_resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(text_input_resource, &text_input_impl,
		NULL, text_input_resource_destroy);

	struct nx_seat_client *seat_client = nx_seat_client_from_resource(seat);
	if (seat_client == NULL) {
		return;
	}

	struct nx_text_input_v3 *text_input = calloc(1, sizeof(*text_input));
	if (text_input == NULL) {
		wl_client_post_no_memory(client);
		return;
	}

	wl_signal_init(&text_input->events.enable);
	wl_signal_init(&text_input->events.commit);
	wl_signal_init(&text_input->events.disable);
	wl_signal_init(&text_input->events.destroy);

	text_input->resource = text_input_resource;
	wl_resource_set_user_data(text_input_resource, text_input);

	wl_signal_add(&seat_client->events.destroy, &text_input->seat_destroy);
	text_input->seat_destroy.notify = text_input_handle_seat_destroy;
	text_input->surface_destroy.notify =
		text_input_handle_focused_surface_destroy;
	wl_list_init(&text_input->surface_destroy.link);

	struct nx_text_input_manager_v3 *manager =
		text_input_manager_from_resource(resource);
	wl_signal_emit_mutable(&manager->events.new_text_input, text_input);
}

static const struct zwp_text_input_manager_v3_interface
		text_input_manager_impl = {
	.destroy = text_input_manager_destroy,
	.get_text_input = text_input_manager_get_text_input,
};

static void text_input_manager_bind(struct wl_client *wl_client, void *data,
		uint32_t version, uint32_t id) {
	struct nx_text_input_manager_v3 *manager = data;

	struct wl_resource *resource = wl_resource_create(wl_client,
		&zwp_text_input_manager_v3_interface, version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(wl_client);
		return;
	}
	wl_resource_set_implementation(resource, &text_input_manager_impl,
		manager, NULL);
}

static void handle_display_destroy(struct wl_listener *listener, void *data) {
	struct nx_text_input_manager_v3 *manager =
		wl_container_of(listener, manager, display_destroy);

	assert(wl_list_empty(&manager->events.new_text_input.listener_list));

	wl_list_remove(&manager->display_destroy.link);
	wl_global_destroy(manager->global);
	free(manager);
}

struct nx_text_input_manager_v3 *nx_text_input_manager_v3_create(
		struct wl_display *display) {
	struct nx_text_input_manager_v3 *manager = calloc(1, sizeof(*manager));
	if (!manager) {
		return NULL;
	}

	wl_signal_init(&manager->events.new_text_input);

	manager->global = wl_global_create(display,
		&zwp_text_input_manager_v3_interface, 1, manager,
		text_input_manager_bind);
	if (!manager->global) {
		free(manager);
		return NULL;
	}

	manager->display_destroy.notify = handle_display_destroy;
	wl_display_add_destroy_listener(display, &manager->display_destroy);

	return manager;
}
