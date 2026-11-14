#include <assert.h>
#include <stdlib.h>
#include <wayland-util.h>
#include "platform/input/tablet/pad_device.h"
#include "platform/input/tablet/pad_group.h"
#include "platform/input/tablet/tablet_v2.h"

static void handle_tablet_pad_v2_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

// Feedback is not shown anywhere.
static void handle_tablet_pad_v2_set_feedback(struct wl_client *client,
		struct wl_resource *resource, uint32_t button,
		const char *description, uint32_t serial) {
}

static const struct zwp_tablet_pad_v2_interface tablet_pad_impl = {
	.set_feedback = handle_tablet_pad_v2_set_feedback,
	.destroy = handle_tablet_pad_v2_destroy,
};

struct nx_tablet_pad_client_v2 *tablet_pad_client_from_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &zwp_tablet_pad_v2_interface,
		&tablet_pad_impl));
	return wl_resource_get_user_data(resource);
}

void destroy_tablet_pad_v2(struct wl_resource *resource) {
	struct nx_tablet_pad_client_v2 *pad =
		tablet_pad_client_from_resource(resource);
	if (!pad) {
		return;
	}

	wl_list_remove(&pad->seat_link);
	wl_list_remove(&pad->pad_link);

	// Child resources become inert.
	input_tablet_pad_destroy_groups(pad);

	if (pad->pad->current_client == pad) {
		pad->pad->current_client = NULL;
	}
	free(pad);
	wl_resource_set_user_data(resource, NULL);
}

static bool alloc_pad_client_arrays(struct nx_tablet_pad_client_v2 *client,
		struct nx_tablet_pad *nx_pad) {
	client->groups = calloc(wl_list_length(&nx_pad->groups),
		sizeof(*client->groups));
	if (!client->groups) {
		return false;
	}

	client->rings = calloc(nx_pad->ring_count, sizeof(*client->rings));
	if (!client->rings) {
		free(client->groups);
		return false;
	}

	client->strips = calloc(nx_pad->strip_count, sizeof(*client->strips));
	if (!client->strips) {
		free(client->groups);
		free(client->rings);
		return false;
	}
	return true;
}

static void send_pad_description(struct nx_tablet_pad_client_v2 *client,
		struct nx_tablet_v2_tablet_pad *pad) {
	if (pad->nx_pad->button_count) {
		zwp_tablet_pad_v2_send_buttons(client->resource, pad->nx_pad->button_count);
	}

	const char **path_ptr;
	wl_array_for_each(path_ptr, &pad->nx_pad->paths) {
		zwp_tablet_pad_v2_send_path(client->resource, *path_ptr);
	}

	size_t i = 0;
	struct nx_tablet_pad_group *group;
	client->group_count = pad->group_count;
	wl_list_for_each(group, &pad->nx_pad->groups, link) {
		input_tablet_pad_add_group(client, group, i++);
	}

	zwp_tablet_pad_v2_send_done(client->resource);
}

void add_tablet_pad_client(struct nx_tablet_seat_client_v2 *seat,
		struct nx_tablet_v2_tablet_pad *pad) {
	struct nx_tablet_pad_client_v2 *client = calloc(1, sizeof(*client));
	if (!client) {
		wl_client_post_no_memory(seat->wl_client);
		return;
	}
	client->pad = pad;
	client->seat = seat;

	if (!alloc_pad_client_arrays(client, pad->nx_pad)) {
		wl_client_post_no_memory(seat->wl_client);
		free(client);
		return;
	}

	uint32_t version = wl_resource_get_version(seat->resource);
	client->resource = wl_resource_create(seat->wl_client,
		&zwp_tablet_pad_v2_interface, version, 0);
	if (!client->resource) {
		wl_client_post_no_memory(seat->wl_client);
		free(client->groups);
		free(client->rings);
		free(client->strips);
		free(client);
		return;
	}
	wl_resource_set_implementation(client->resource, &tablet_pad_impl,
		client, destroy_tablet_pad_v2);
	zwp_tablet_seat_v2_send_pad_added(seat->resource, client->resource);
	client->client = seat->wl_client;

	send_pad_description(client, pad);

	wl_list_insert(&seat->pads, &client->seat_link);
	wl_list_insert(&pad->clients, &client->pad_link);
}

static void handle_nx_tablet_pad_destroy(struct wl_listener *listener, void *data) {
	struct nx_tablet_v2_tablet_pad *pad =
		wl_container_of(listener, pad, pad_destroy);

	struct nx_tablet_pad_client_v2 *client, *next;
	wl_list_for_each_safe(client, next, &pad->clients, pad_link) {
		zwp_tablet_pad_v2_send_removed(client->resource);
		destroy_tablet_pad_v2(client->resource);
	}

	wl_list_remove(&pad->clients);
	wl_list_remove(&pad->link);
	wl_list_remove(&pad->pad_destroy.link);
	free(pad->groups);
	free(pad);
}

struct nx_tablet_v2_tablet_pad *nx_tablet_pad_create(
		struct nx_tablet_manager_v2 *manager,
		struct nx_seat *nx_seat,
		struct nx_input_device *nx_device) {
	assert(nx_device->type == NX_INPUT_DEVICE_TABLET_PAD);
	struct nx_tablet_seat_v2 *seat = get_or_create_tablet_seat(manager, nx_seat);
	if (!seat) {
		return NULL;
	}
	struct nx_tablet_pad *nx_pad = nx_tablet_pad_from_input_device(nx_device);
	struct nx_tablet_v2_tablet_pad *pad = calloc(1, sizeof(*pad));
	if (!pad) {
		return NULL;
	}

	pad->group_count = wl_list_length(&nx_pad->groups);
	pad->groups = calloc(pad->group_count, sizeof(uint32_t));
	if (!pad->groups) {
		free(pad);
		return NULL;
	}

	pad->nx_pad = nx_pad;
	wl_list_init(&pad->clients);

	pad->pad_destroy.notify = handle_nx_tablet_pad_destroy;
	wl_signal_add(&nx_device->events.destroy, &pad->pad_destroy);
	wl_list_insert(&seat->pads, &pad->link);

	struct nx_tablet_seat_client_v2 *pos;
	wl_list_for_each(pos, &seat->clients, seat_link) {
		add_tablet_pad_client(pos, pad);
	}

	return pad;
}
