#include <assert.h>
#include <stdlib.h>
#include <wayland-server-core.h>
#include "platform/input/seat/seat.h"
#include "platform/input/tablet/tablet_v2.h"

#define TABLET_MANAGER_VERSION 1

struct nx_tablet_manager_client_v2 {
	struct nx_tablet_manager_v2 *manager;

	struct wl_list tablet_seats; // nx_tablet_seat_client_v2.client_link
};

static void tablet_seat_destroy(struct nx_tablet_seat_v2 *seat) {
	struct nx_tablet_seat_client_v2 *client, *next;
	wl_list_for_each_safe(client, next, &seat->clients, seat_link) {
		tablet_seat_client_v2_destroy(client->resource);
	}

	wl_list_remove(&seat->link);
	wl_list_remove(&seat->seat_destroy.link);
	free(seat);
}

static void handle_nx_seat_destroy(struct wl_listener *listener, void *data) {
	struct nx_tablet_seat_v2 *seat =
		wl_container_of(listener, seat, seat_destroy);
	tablet_seat_destroy(seat);
}

static struct nx_tablet_seat_v2 *create_tablet_seat(
		struct nx_tablet_manager_v2 *manager,
		struct nx_seat *nx_seat) {
	struct nx_tablet_seat_v2 *tablet_seat = calloc(1, sizeof(*tablet_seat));
	if (!tablet_seat) {
		return NULL;
	}

	tablet_seat->nx_seat = nx_seat;

	wl_list_init(&tablet_seat->clients);

	wl_list_init(&tablet_seat->tablets);
	wl_list_init(&tablet_seat->tools);
	wl_list_init(&tablet_seat->pads);

	tablet_seat->seat_destroy.notify = handle_nx_seat_destroy;
	wl_signal_add(&nx_seat->events.destroy, &tablet_seat->seat_destroy);

	wl_list_insert(&manager->seats, &tablet_seat->link);
	return tablet_seat;
}

struct nx_tablet_seat_v2 *get_or_create_tablet_seat(
		struct nx_tablet_manager_v2 *manager,
		struct nx_seat *nx_seat) {
	struct nx_tablet_seat_v2 *pos;
	wl_list_for_each(pos, &manager->seats, link) {
		if (pos->nx_seat == nx_seat) {
			return pos;
		}
	}

	return create_tablet_seat(manager, nx_seat);
}

static void tablet_seat_handle_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct zwp_tablet_seat_v2_interface seat_impl = {
	.destroy = tablet_seat_handle_destroy,
};

static struct nx_tablet_seat_client_v2 *tablet_seat_client_from_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &zwp_tablet_seat_v2_interface,
		&seat_impl));
	return wl_resource_get_user_data(resource);
}

void tablet_seat_client_v2_destroy(struct wl_resource *resource) {
	struct nx_tablet_seat_client_v2 *seat = tablet_seat_client_from_resource(resource);
	if (!seat) {
		return;
	}

	struct nx_tablet_client_v2 *tablet, *next_tablet;
	wl_list_for_each_safe(tablet, next_tablet, &seat->tablets, seat_link) {
		destroy_tablet_v2(tablet->resource);
	}

	struct nx_tablet_pad_client_v2 *pad, *next_pad;
	wl_list_for_each_safe(pad, next_pad, &seat->pads, seat_link) {
		destroy_tablet_pad_v2(pad->resource);
	}

	struct nx_tablet_tool_client_v2 *tool, *next_tool;
	wl_list_for_each_safe(tool, next_tool, &seat->tools, seat_link) {
		destroy_tablet_tool_v2(tool->resource);
	}

	wl_list_remove(&seat->seat_link);
	wl_list_remove(&seat->client_link);
	wl_list_remove(&seat->seat_client_destroy.link);

	free(seat);
	wl_resource_set_user_data(resource, NULL);
}

static void handle_seat_client_destroy(struct wl_listener *listener, void *data) {
	struct nx_tablet_seat_client_v2 *seat =
		wl_container_of(listener, seat, seat_client_destroy);
	tablet_seat_client_v2_destroy(seat->resource);
}

// Announces the seat's existing devices.
static void add_seat_devices(struct nx_tablet_seat_client_v2 *seat_client,
		struct nx_tablet_seat_v2 *tablet_seat) {
	struct nx_tablet_v2_tablet *tablet_pos;
	wl_list_for_each(tablet_pos, &tablet_seat->tablets, link) {
		add_tablet_client(seat_client, tablet_pos);
	}

	struct nx_tablet_v2_tablet_pad *pad_pos;
	wl_list_for_each(pad_pos, &tablet_seat->pads, link) {
		add_tablet_pad_client(seat_client, pad_pos);
	}

	struct nx_tablet_v2_tablet_tool *tool_pos;
	wl_list_for_each(tool_pos, &tablet_seat->tools, link) {
		add_tablet_tool_client(seat_client, tool_pos);
	}
}

static const struct zwp_tablet_manager_v2_interface manager_impl;

static struct nx_tablet_manager_client_v2 *tablet_manager_client_from_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &zwp_tablet_manager_v2_interface,
		&manager_impl));
	return wl_resource_get_user_data(resource);
}

static void get_tablet_seat(struct wl_client *wl_client, struct wl_resource *resource,
		uint32_t id, struct wl_resource *seat_resource) {
	struct nx_tablet_manager_client_v2 *manager =
		tablet_manager_client_from_resource(resource);
	struct wl_resource *tablet_seat_resource = wl_resource_create(wl_client,
		&zwp_tablet_seat_v2_interface, TABLET_MANAGER_VERSION, id);
	if (tablet_seat_resource == NULL) {
		wl_client_post_no_memory(wl_client);
		return;
	}
	wl_resource_set_implementation(tablet_seat_resource, &seat_impl, NULL,
		tablet_seat_client_v2_destroy);

	struct nx_seat_client *seat = nx_seat_client_from_resource(seat_resource);
	if (seat == NULL) {
		return;
	}
	struct nx_tablet_seat_v2 *tablet_seat =
		get_or_create_tablet_seat(manager->manager, seat->seat);
	if (!tablet_seat) {
		wl_client_post_no_memory(wl_client);
		return;
	}

	struct nx_tablet_seat_client_v2 *seat_client = calloc(1, sizeof(*seat_client));
	if (seat_client == NULL) {
		wl_client_post_no_memory(wl_client);
		return;
	}

	seat_client->resource = tablet_seat_resource;
	seat_client->seat_client = seat;
	seat_client->wl_client = wl_client;
	wl_list_init(&seat_client->tools);
	wl_list_init(&seat_client->tablets);
	wl_list_init(&seat_client->pads);
	wl_resource_set_user_data(tablet_seat_resource, seat_client);

	seat_client->seat_client_destroy.notify = handle_seat_client_destroy;
	wl_signal_add(&seat->events.destroy, &seat_client->seat_client_destroy);

	wl_list_insert(&manager->tablet_seats, &seat_client->client_link);
	wl_list_insert(&tablet_seat->clients, &seat_client->seat_link);

	add_seat_devices(seat_client, tablet_seat);
}

static void tablet_manager_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct zwp_tablet_manager_v2_interface manager_impl = {
	.get_tablet_seat = get_tablet_seat,
	.destroy = tablet_manager_destroy,
};

static void tablet_manager_client_destroy(struct wl_resource *resource) {
	struct nx_tablet_manager_client_v2 *client =
		tablet_manager_client_from_resource(resource);

	struct nx_tablet_seat_client_v2 *pos, *next;
	wl_list_for_each_safe(pos, next, &client->tablet_seats, client_link) {
		tablet_seat_client_v2_destroy(pos->resource);
	}

	free(client);
	wl_resource_set_user_data(resource, NULL);
}

static void tablet_v2_bind(struct wl_client *wl_client, void *data,
		uint32_t version, uint32_t id) {
	struct nx_tablet_manager_v2 *manager = data;

	struct nx_tablet_manager_client_v2 *client = calloc(1, sizeof(*client));
	if (client == NULL) {
		wl_client_post_no_memory(wl_client);
		return;
	}

	wl_list_init(&client->tablet_seats);

	struct wl_resource *resource =
		wl_resource_create(wl_client, &zwp_tablet_manager_v2_interface, version, id);
	if (resource == NULL) {
		free(client);
		wl_client_post_no_memory(wl_client);
		return;
	}
	client->manager = manager;

	wl_resource_set_implementation(resource, &manager_impl, client,
		tablet_manager_client_destroy);
}

static void handle_display_destroy(struct wl_listener *listener, void *data) {
	struct nx_tablet_manager_v2 *manager =
		wl_container_of(listener, manager, display_destroy);

	wl_list_remove(&manager->display_destroy.link);

	struct nx_tablet_seat_v2 *seat, *next;
	wl_list_for_each_safe(seat, next, &manager->seats, link) {
		tablet_seat_destroy(seat);
	}

	wl_global_destroy(manager->wl_global);
	free(manager);
}

struct nx_tablet_manager_v2 *nx_tablet_v2_create(struct wl_display *display) {
	struct nx_tablet_manager_v2 *tablet = calloc(1, sizeof(*tablet));
	if (!tablet) {
		return NULL;
	}

	tablet->wl_global = wl_global_create(display,
		&zwp_tablet_manager_v2_interface, TABLET_MANAGER_VERSION,
		tablet, tablet_v2_bind);
	if (tablet->wl_global == NULL) {
		free(tablet);
		return NULL;
	}

	wl_list_init(&tablet->seats);

	tablet->display_destroy.notify = handle_display_destroy;
	wl_display_add_destroy_listener(display, &tablet->display_destroy);

	return tablet;
}
