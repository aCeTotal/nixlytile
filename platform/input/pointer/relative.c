#include <assert.h>
#include <stdlib.h>
#include <wayland-server-core.h>
#include "platform/input/pointer/relative.h"
#include "platform/input/seat/seat.h"
#include "relative-pointer-unstable-v1-protocol.h"

#define RELATIVE_POINTER_MANAGER_VERSION 1

static const struct zwp_relative_pointer_manager_v1_interface relative_pointer_manager_v1_impl;
static const struct zwp_relative_pointer_v1_interface relative_pointer_v1_impl;

// NULL for inert relative pointers.
static struct nx_relative_pointer_v1 *relative_pointer_from_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &zwp_relative_pointer_v1_interface,
		&relative_pointer_v1_impl));
	return wl_resource_get_user_data(resource);
}

static struct nx_relative_pointer_manager_v1 *relative_pointer_manager_from_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &zwp_relative_pointer_manager_v1_interface,
		&relative_pointer_manager_v1_impl));
	return wl_resource_get_user_data(resource);
}

static void relative_pointer_destroy(struct nx_relative_pointer_v1 *relative_pointer) {
	wl_list_remove(&relative_pointer->link);
	wl_list_remove(&relative_pointer->seat_destroy.link);
	wl_list_remove(&relative_pointer->pointer_destroy.link);

	wl_resource_set_user_data(relative_pointer->resource, NULL);
	free(relative_pointer);
}

static void relative_pointer_v1_handle_resource_destroy(struct wl_resource *resource) {
	struct nx_relative_pointer_v1 *relative_pointer =
		relative_pointer_from_resource(resource);
	if (relative_pointer == NULL) {
		return;
	}
	relative_pointer_destroy(relative_pointer);
}

static void relative_pointer_v1_handle_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static void relative_pointer_handle_seat_destroy(struct wl_listener *listener,
		void *data) {
	struct nx_relative_pointer_v1 *relative_pointer =
		wl_container_of(listener, relative_pointer, seat_destroy);
	relative_pointer_destroy(relative_pointer);
}

static void relative_pointer_handle_pointer_destroy(struct wl_listener *listener,
		void *data) {
	struct nx_relative_pointer_v1 *relative_pointer =
		wl_container_of(listener, relative_pointer, pointer_destroy);
	relative_pointer_destroy(relative_pointer);
}

static void relative_pointer_manager_v1_handle_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static void relative_pointer_manager_v1_handle_get_relative_pointer(
		struct wl_client *client, struct wl_resource *resource, uint32_t id,
		struct wl_resource *pointer) {
	struct nx_relative_pointer_manager_v1 *manager =
		relative_pointer_manager_from_resource(resource);
	struct nx_seat_client *seat_client =
		nx_seat_client_from_pointer_resource(pointer);

	struct wl_resource *relative_pointer_resource = wl_resource_create(client,
		&zwp_relative_pointer_v1_interface, wl_resource_get_version(resource), id);
	if (relative_pointer_resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(relative_pointer_resource, &relative_pointer_v1_impl,
		NULL, relative_pointer_v1_handle_resource_destroy);

	if (seat_client == NULL) {
		return;
	}

	struct nx_relative_pointer_v1 *relative_pointer = calloc(1, sizeof(*relative_pointer));
	if (relative_pointer == NULL) {
		wl_client_post_no_memory(client);
		return;
	}

	relative_pointer->resource = relative_pointer_resource;
	relative_pointer->pointer_resource = pointer;

	relative_pointer->seat = seat_client->seat;
	wl_signal_add(&relative_pointer->seat->events.destroy,
		&relative_pointer->seat_destroy);
	relative_pointer->seat_destroy.notify = relative_pointer_handle_seat_destroy;

	wl_resource_set_user_data(relative_pointer_resource, relative_pointer);

	wl_list_insert(&manager->relative_pointers, &relative_pointer->link);

	wl_resource_add_destroy_listener(relative_pointer->pointer_resource,
		&relative_pointer->pointer_destroy);
	relative_pointer->pointer_destroy.notify = relative_pointer_handle_pointer_destroy;
}

static void relative_pointer_manager_v1_bind(struct wl_client *wl_client, void *data,
		uint32_t version, uint32_t id) {
	struct nx_relative_pointer_manager_v1 *manager = data;

	struct wl_resource *manager_resource = wl_resource_create(wl_client,
		&zwp_relative_pointer_manager_v1_interface, version, id);
	if (manager_resource == NULL) {
		wl_client_post_no_memory(wl_client);
		return;
	}

	wl_resource_set_implementation(manager_resource, &relative_pointer_manager_v1_impl,
		manager, NULL);
}

static void handle_display_destroy(struct wl_listener *listener, void *data) {
	struct nx_relative_pointer_manager_v1 *manager =
		wl_container_of(listener, manager, display_destroy_listener);
	wl_list_remove(&manager->display_destroy_listener.link);
	wl_global_destroy(manager->global);
	free(manager);
}

static const struct zwp_relative_pointer_manager_v1_interface relative_pointer_manager_v1_impl = {
	.destroy = relative_pointer_manager_v1_handle_destroy,
	.get_relative_pointer = relative_pointer_manager_v1_handle_get_relative_pointer,
};

static const struct zwp_relative_pointer_v1_interface relative_pointer_v1_impl = {
	.destroy = relative_pointer_v1_handle_destroy,
};

struct nx_relative_pointer_manager_v1 *nx_relative_pointer_manager_v1_create(
		struct wl_display *display) {
	struct nx_relative_pointer_manager_v1 *manager = calloc(1, sizeof(*manager));
	if (manager == NULL) {
		return NULL;
	}

	wl_list_init(&manager->relative_pointers);

	manager->global = wl_global_create(display,
		&zwp_relative_pointer_manager_v1_interface, RELATIVE_POINTER_MANAGER_VERSION,
		manager, relative_pointer_manager_v1_bind);
	if (manager->global == NULL) {
		free(manager);
		return NULL;
	}

	manager->display_destroy_listener.notify = handle_display_destroy;
	wl_display_add_destroy_listener(display, &manager->display_destroy_listener);

	return manager;
}

void nx_relative_pointer_manager_v1_send_relative_motion(
		struct nx_relative_pointer_manager_v1 *manager, struct nx_seat *seat,
		uint64_t time_usec, double dx, double dy,
		double dx_unaccel, double dy_unaccel) {
	struct nx_seat_client *focused = seat->pointer_state.focused_client;
	if (focused == NULL) {
		return;
	}

	struct nx_relative_pointer_v1 *pointer;
	wl_list_for_each(pointer, &manager->relative_pointers, link) {
		if (pointer->seat != seat || nx_seat_client_from_pointer_resource(
				pointer->pointer_resource) != focused) {
			continue;
		}

		zwp_relative_pointer_v1_send_relative_motion(pointer->resource,
			(uint32_t)(time_usec >> 32), (uint32_t)time_usec,
			wl_fixed_from_double(dx), wl_fixed_from_double(dy),
			wl_fixed_from_double(dx_unaccel), wl_fixed_from_double(dy_unaccel));
	}
}
