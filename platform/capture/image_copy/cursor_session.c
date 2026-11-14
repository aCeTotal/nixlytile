#include <assert.h>
#include <stdlib.h>
#include "platform/capture/image_copy/session.h"
#include "platform/input/seat/seat.h"
#include "ext-image-copy-capture-v1-protocol.h"

struct cursor_session {
	struct wl_resource *resource;
	struct capture_cursor_source *source;
	bool capture_session_created;

	// State last sent to client.
	struct {
		bool entered;
		int32_t x, y;
		struct {
			int32_t x, y;
		} hotspot;
	} prev;

	struct wl_listener source_destroy;
	struct wl_listener source_update;
};

static const struct ext_image_copy_capture_cursor_session_v1_interface
	cursor_session_impl;

static struct cursor_session *cursor_session_from_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource,
		&ext_image_copy_capture_cursor_session_v1_interface,
		&cursor_session_impl));
	return wl_resource_get_user_data(resource);
}

static void cursor_session_destroy(struct cursor_session *cursor_session) {
	if (cursor_session == NULL) {
		return;
	}
	if (cursor_session->source->entered) {
		ext_image_copy_capture_cursor_session_v1_send_leave(
			cursor_session->resource);
	}
	wl_resource_set_user_data(cursor_session->resource, NULL);
	wl_list_remove(&cursor_session->source_destroy.link);
	wl_list_remove(&cursor_session->source_update.link);
	free(cursor_session);
}

static void cursor_session_handle_destroy(struct wl_client *client,
		struct wl_resource *cursor_session_resource) {
	wl_resource_destroy(cursor_session_resource);
}

static void cursor_session_handle_get_capture_session(struct wl_client *client,
		struct wl_resource *cursor_session_resource, uint32_t new_id) {
	struct cursor_session *cursor_session =
		cursor_session_from_resource(cursor_session_resource);
	if (cursor_session == NULL) {
		image_copy_session_create(cursor_session_resource, new_id, NULL, 0);
		return;
	}

	if (cursor_session->capture_session_created) {
		wl_resource_post_error(cursor_session_resource,
			EXT_IMAGE_COPY_CAPTURE_CURSOR_SESSION_V1_ERROR_DUPLICATE_SESSION,
			"get_capture_session sent twice");
		return;
	}

	cursor_session->capture_session_created = true;
	image_copy_session_create(cursor_session_resource, new_id,
		&cursor_session->source->base, 0);
}

static const struct ext_image_copy_capture_cursor_session_v1_interface
		cursor_session_impl = {
	.destroy = cursor_session_handle_destroy,
	.get_capture_session = cursor_session_handle_get_capture_session,
};

static void cursor_session_handle_resource_destroy(struct wl_resource *resource) {
	cursor_session_destroy(cursor_session_from_resource(resource));
}

static void cursor_session_handle_source_destroy(struct wl_listener *listener,
		void *data) {
	struct cursor_session *cursor_session =
		wl_container_of(listener, cursor_session, source_destroy);
	cursor_session_destroy(cursor_session);
}

static void cursor_session_update(struct cursor_session *cursor_session) {
	struct capture_cursor_source *cursor_source = cursor_session->source;
	struct wl_resource *resource = cursor_session->resource;

	if (cursor_source->entered && !cursor_session->prev.entered) {
		ext_image_copy_capture_cursor_session_v1_send_enter(resource);
	}
	if (!cursor_source->entered && cursor_session->prev.entered) {
		ext_image_copy_capture_cursor_session_v1_send_leave(resource);
	}

	if (cursor_source->x != cursor_session->prev.x ||
			cursor_source->y != cursor_session->prev.y) {
		ext_image_copy_capture_cursor_session_v1_send_position(resource,
			cursor_source->x, cursor_source->y);
	}

	if (cursor_source->hotspot.x != cursor_session->prev.hotspot.x ||
			cursor_source->hotspot.y != cursor_session->prev.hotspot.y) {
		ext_image_copy_capture_cursor_session_v1_send_hotspot(resource,
			cursor_source->hotspot.x, cursor_source->hotspot.y);
	}

	cursor_session->prev.entered = cursor_source->entered;
	cursor_session->prev.x = cursor_source->x;
	cursor_session->prev.y = cursor_source->y;
	cursor_session->prev.hotspot.y = cursor_source->hotspot.y;
}

static void cursor_session_handle_source_update(struct wl_listener *listener,
		void *data) {
	struct cursor_session *cursor_session =
		wl_container_of(listener, cursor_session, source_update);
	cursor_session_update(cursor_session);
}

static struct capture_cursor_source *pointer_cursor_source(
		struct wl_resource *source_resource,
		struct wl_resource *pointer_resource) {
	struct capture_source *source =
		capture_source_from_resource(source_resource);
	struct nx_seat_client *seat_client =
		nx_seat_client_from_pointer_resource(pointer_resource);
	struct nx_seat *seat = seat_client != NULL ? seat_client->seat : NULL;

	if (source == NULL || seat == NULL ||
			source->impl->get_pointer_cursor == NULL) {
		return NULL;
	}
	return source->impl->get_pointer_cursor(source, seat);
}

void image_copy_cursor_session_create(struct wl_resource *manager_resource,
		uint32_t new_id, struct wl_resource *source_resource,
		struct wl_resource *pointer_resource) {
	struct capture_cursor_source *source_cursor =
		pointer_cursor_source(source_resource, pointer_resource);

	struct wl_resource *cursor_session_resource = wl_resource_create(
		wl_resource_get_client(manager_resource),
		&ext_image_copy_capture_cursor_session_v1_interface,
		wl_resource_get_version(manager_resource), new_id);
	if (cursor_session_resource == NULL) {
		wl_resource_post_no_memory(manager_resource);
		return;
	}
	wl_resource_set_implementation(cursor_session_resource,
		&cursor_session_impl, NULL, cursor_session_handle_resource_destroy);

	if (source_cursor == NULL) {
		return;
	}

	struct cursor_session *cursor_session = calloc(1, sizeof(*cursor_session));
	if (cursor_session == NULL) {
		wl_resource_post_no_memory(manager_resource);
		return;
	}

	cursor_session->resource = cursor_session_resource;
	cursor_session->source = source_cursor;

	cursor_session->source_destroy.notify = cursor_session_handle_source_destroy;
	wl_signal_add(&source_cursor->base.events.destroy,
		&cursor_session->source_destroy);

	cursor_session->source_update.notify = cursor_session_handle_source_update;
	wl_signal_add(&source_cursor->events.update, &cursor_session->source_update);

	wl_resource_set_user_data(cursor_session_resource, cursor_session);

	cursor_session_update(cursor_session);
}
