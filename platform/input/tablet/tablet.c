#include <assert.h>
#include <stdlib.h>
#include <wayland-util.h>
#include "platform/input/tablet/tablet_v2.h"
#include "platform/input/tablet/tool_device.h"

static const struct zwp_tablet_v2_interface tablet_impl;

static struct nx_tablet_client_v2 *tablet_client_from_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &zwp_tablet_v2_interface,
		&tablet_impl));
	return wl_resource_get_user_data(resource);
}

void destroy_tablet_v2(struct wl_resource *resource) {
	struct nx_tablet_client_v2 *tablet = tablet_client_from_resource(resource);
	if (!tablet) {
		return;
	}

	wl_list_remove(&tablet->seat_link);
	wl_list_remove(&tablet->tablet_link);
	free(tablet);
	wl_resource_set_user_data(resource, NULL);
}

static void handle_tablet_v2_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct zwp_tablet_v2_interface tablet_impl = {
	.destroy = handle_tablet_v2_destroy,
};

static void handle_nx_tablet_destroy(struct wl_listener *listener, void *data) {
	struct nx_tablet_v2_tablet *tablet =
		wl_container_of(listener, tablet, tablet_destroy);

	struct nx_tablet_client_v2 *pos, *next;
	wl_list_for_each_safe(pos, next, &tablet->clients, tablet_link) {
		zwp_tablet_v2_send_removed(pos->resource);
	}

	wl_list_remove(&tablet->clients);
	wl_list_remove(&tablet->link);
	wl_list_remove(&tablet->tablet_destroy.link);
	free(tablet);
}

struct nx_tablet_v2_tablet *nx_tablet_create(
		struct nx_tablet_manager_v2 *manager,
		struct nx_seat *nx_seat,
		struct nx_input_device *nx_device) {
	assert(nx_device->type == NX_INPUT_DEVICE_TABLET);
	struct nx_tablet_seat_v2 *seat = get_or_create_tablet_seat(manager, nx_seat);
	if (!seat) {
		return NULL;
	}
	struct nx_tablet_v2_tablet *tablet = calloc(1, sizeof(*tablet));
	if (!tablet) {
		return NULL;
	}

	tablet->nx_tablet = nx_tablet_from_input_device(nx_device);
	wl_list_init(&tablet->clients);

	tablet->tablet_destroy.notify = handle_nx_tablet_destroy;
	wl_signal_add(&nx_device->events.destroy, &tablet->tablet_destroy);
	wl_list_insert(&seat->tablets, &tablet->link);

	struct nx_tablet_seat_client_v2 *pos;
	wl_list_for_each(pos, &seat->clients, seat_link) {
		add_tablet_client(pos, tablet);
	}

	return tablet;
}

void add_tablet_client(struct nx_tablet_seat_client_v2 *seat,
		struct nx_tablet_v2_tablet *tablet) {
	struct nx_tablet_client_v2 *client = calloc(1, sizeof(*client));
	if (!client) {
		return;
	}

	uint32_t version = wl_resource_get_version(seat->resource);
	client->resource =
		wl_resource_create(seat->wl_client, &zwp_tablet_v2_interface,
			version, 0);
	if (!client->resource) {
		wl_resource_post_no_memory(seat->resource);
		free(client);
		return;
	}
	wl_resource_set_implementation(client->resource, &tablet_impl,
		client, destroy_tablet_v2);
	zwp_tablet_seat_v2_send_tablet_added(seat->resource, client->resource);

	struct nx_tablet *nx_tablet = tablet->nx_tablet;
	if (nx_tablet->base.name) {
		zwp_tablet_v2_send_name(client->resource, nx_tablet->base.name);
	}
	if (nx_tablet->usb_vendor_id != 0) {
		zwp_tablet_v2_send_id(client->resource,
			nx_tablet->usb_vendor_id, nx_tablet->usb_product_id);
	}

	const char **path_ptr;
	wl_array_for_each(path_ptr, &nx_tablet->paths) {
		zwp_tablet_v2_send_path(client->resource, *path_ptr);
	}

	zwp_tablet_v2_send_done(client->resource);

	client->client = seat->wl_client;
	wl_list_insert(&seat->tablets, &client->seat_link);
	wl_list_insert(&tablet->clients, &client->tablet_link);
}
