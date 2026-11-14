#include <assert.h>
#include <stdlib.h>
#include <wayland-server-core.h>
#include "platform/input/pointer/gestures.h"
#include "pointer-gestures-unstable-v1-protocol.h"

#define POINTER_GESTURES_VERSION 3

static const struct zwp_pointer_gestures_v1_interface gestures_impl;

static struct nx_pointer_gestures_v1 *pointer_gestures_from_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource,
		&zwp_pointer_gestures_v1_interface, &gestures_impl));
	return wl_resource_get_user_data(resource);
}

static void resource_handle_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static void resource_remove_from_list(struct wl_resource *resource) {
	wl_list_remove(wl_resource_get_link(resource));
}

static const struct zwp_pointer_gesture_swipe_v1_interface swipe_impl = {
	.destroy = resource_handle_destroy,
};

static const struct zwp_pointer_gesture_pinch_v1_interface pinch_impl = {
	.destroy = resource_handle_destroy,
};

static const struct zwp_pointer_gesture_hold_v1_interface hold_impl = {
	.destroy = resource_handle_destroy,
};

struct gesture_kind {
	const struct wl_interface *interface;
	const void *impl;
};

static const struct gesture_kind swipe_kind = {
	&zwp_pointer_gesture_swipe_v1_interface, &swipe_impl,
};

static const struct gesture_kind pinch_kind = {
	&zwp_pointer_gesture_pinch_v1_interface, &pinch_impl,
};

static const struct gesture_kind hold_kind = {
	&zwp_pointer_gesture_hold_v1_interface, &hold_impl,
};

static struct wl_resource *create_gesture(struct wl_resource *gestures_resource,
		uint32_t id, const struct gesture_kind *kind) {
	struct wl_client *client = wl_resource_get_client(gestures_resource);
	struct wl_resource *gesture = wl_resource_create(client, kind->interface,
		wl_resource_get_version(gestures_resource), id);
	if (gesture == NULL) {
		wl_client_post_no_memory(client);
		return NULL;
	}
	wl_resource_set_implementation(gesture, kind->impl, NULL,
		resource_remove_from_list);
	return gesture;
}

// Seatless pointers give inert gestures.
static void add_gesture(struct wl_list *list, struct wl_resource *gesture,
		struct wl_resource *pointer_resource) {
	struct nx_seat_client *seat_client =
		nx_seat_client_from_pointer_resource(pointer_resource);
	wl_resource_set_user_data(gesture,
		seat_client != NULL ? seat_client->seat : NULL);
	wl_list_insert(list, wl_resource_get_link(gesture));
}

static void get_swipe_gesture(struct wl_client *client,
		struct wl_resource *gestures_resource, uint32_t id,
		struct wl_resource *pointer_resource) {
	struct nx_pointer_gestures_v1 *gestures =
		pointer_gestures_from_resource(gestures_resource);
	struct wl_resource *gesture =
		create_gesture(gestures_resource, id, &swipe_kind);
	if (gesture != NULL) {
		add_gesture(&gestures->swipes, gesture, pointer_resource);
	}
}

static void get_pinch_gesture(struct wl_client *client,
		struct wl_resource *gestures_resource, uint32_t id,
		struct wl_resource *pointer_resource) {
	struct nx_pointer_gestures_v1 *gestures =
		pointer_gestures_from_resource(gestures_resource);
	struct wl_resource *gesture =
		create_gesture(gestures_resource, id, &pinch_kind);
	if (gesture != NULL) {
		add_gesture(&gestures->pinches, gesture, pointer_resource);
	}
}

static void get_hold_gesture(struct wl_client *client,
		struct wl_resource *gestures_resource, uint32_t id,
		struct wl_resource *pointer_resource) {
	struct nx_pointer_gestures_v1 *gestures =
		pointer_gestures_from_resource(gestures_resource);
	struct wl_resource *gesture =
		create_gesture(gestures_resource, id, &hold_kind);
	if (gesture != NULL) {
		add_gesture(&gestures->holds, gesture, pointer_resource);
	}
}

static void pointer_gestures_release(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct zwp_pointer_gestures_v1_interface gestures_impl = {
	.get_swipe_gesture = get_swipe_gesture,
	.get_pinch_gesture = get_pinch_gesture,
	.release = pointer_gestures_release,
	.get_hold_gesture = get_hold_gesture,
};

static void pointer_gestures_v1_bind(struct wl_client *wl_client, void *data,
		uint32_t version, uint32_t id) {
	struct nx_pointer_gestures_v1 *gestures = data;

	struct wl_resource *resource = wl_resource_create(wl_client,
		&zwp_pointer_gestures_v1_interface, version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(wl_client);
		return;
	}

	wl_resource_set_implementation(resource, &gestures_impl, gestures, NULL);
}

static void handle_display_destroy(struct wl_listener *listener, void *data) {
	struct nx_pointer_gestures_v1 *gestures =
		wl_container_of(listener, gestures, display_destroy);
	wl_list_remove(&gestures->display_destroy.link);
	wl_global_destroy(gestures->global);
	free(gestures);
}

struct nx_pointer_gestures_v1 *nx_pointer_gestures_v1_create(
		struct wl_display *display) {
	struct nx_pointer_gestures_v1 *gestures = calloc(1, sizeof(*gestures));
	if (!gestures) {
		return NULL;
	}

	wl_list_init(&gestures->swipes);
	wl_list_init(&gestures->pinches);
	wl_list_init(&gestures->holds);

	gestures->global = wl_global_create(display,
		&zwp_pointer_gestures_v1_interface, POINTER_GESTURES_VERSION,
		gestures, pointer_gestures_v1_bind);
	if (gestures->global == NULL) {
		free(gestures);
		return NULL;
	}

	gestures->display_destroy.notify = handle_display_destroy;
	wl_display_add_destroy_listener(display, &gestures->display_destroy);

	return gestures;
}
