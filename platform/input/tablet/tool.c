#include <assert.h>
#include <stdlib.h>
#include "platform/input/tablet/tablet_v2.h"
#include "platform/input/tablet/tool_device.h"
#include "platform/surface/compositor.h"

static void tablet_tool_cursor_surface_handle_commit(struct nx_surface *surface) {
	pixman_region32_clear(&surface->input_region);
	if (nx_surface_has_buffer(surface)) {
		nx_surface_map(surface);
	}
}

static const struct nx_surface_role tablet_tool_cursor_surface_role = {
	.name = "wp_tablet_tool-cursor",
	.no_object = true,
	.commit = tablet_tool_cursor_surface_handle_commit,
};

static void handle_tablet_tool_v2_set_cursor(struct wl_client *client,
		struct wl_resource *resource, uint32_t serial,
		struct wl_resource *surface_resource,
		int32_t hotspot_x, int32_t hotspot_y) {
	struct nx_tablet_tool_client_v2 *tool = tablet_tool_client_from_resource(resource);
	if (!tool || !tool->tool) {
		return;
	}

	struct nx_surface *surface = NULL;
	if (surface_resource != NULL) {
		surface = nx_surface_from_resource(surface_resource);
		if (!nx_surface_set_role(surface, &tablet_tool_cursor_surface_role,
				surface_resource, ZWP_TABLET_TOOL_V2_ERROR_ROLE)) {
			return;
		}
		tablet_tool_cursor_surface_handle_commit(surface);
	}

	struct nx_tablet_v2_event_cursor evt = {
		.surface = surface,
		.serial = serial,
		.hotspot_x = hotspot_x,
		.hotspot_y = hotspot_y,
		.seat_client = tool->seat->seat_client,
	};

	wl_signal_emit_mutable(&tool->tool->events.set_cursor, &evt);
}

static void handle_tablet_tool_v2_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct zwp_tablet_tool_v2_interface tablet_tool_impl = {
	.set_cursor = handle_tablet_tool_v2_set_cursor,
	.destroy = handle_tablet_tool_v2_destroy,
};

static enum zwp_tablet_tool_v2_type tablet_type_from_nx_type(
		enum nx_tablet_tool_type nx_type) {
	switch (nx_type) {
	case NX_TABLET_TOOL_TYPE_PEN:
		return ZWP_TABLET_TOOL_V2_TYPE_PEN;
	case NX_TABLET_TOOL_TYPE_ERASER:
		return ZWP_TABLET_TOOL_V2_TYPE_ERASER;
	case NX_TABLET_TOOL_TYPE_BRUSH:
		return ZWP_TABLET_TOOL_V2_TYPE_BRUSH;
	case NX_TABLET_TOOL_TYPE_PENCIL:
		return ZWP_TABLET_TOOL_V2_TYPE_PENCIL;
	case NX_TABLET_TOOL_TYPE_AIRBRUSH:
		return ZWP_TABLET_TOOL_V2_TYPE_AIRBRUSH;
	case NX_TABLET_TOOL_TYPE_MOUSE:
		return ZWP_TABLET_TOOL_V2_TYPE_MOUSE;
	case NX_TABLET_TOOL_TYPE_LENS:
		return ZWP_TABLET_TOOL_V2_TYPE_LENS;
	default:
		abort(); // totems have no protocol type
	}
}

void destroy_tablet_tool_v2(struct wl_resource *resource) {
	struct nx_tablet_tool_client_v2 *client =
		tablet_tool_client_from_resource(resource);
	if (!client) {
		return;
	}

	if (client->frame_source) {
		wl_event_source_remove(client->frame_source);
	}

	if (client->tool && client->tool->current_client == client) {
		wl_list_remove(&client->tool->surface_destroy.link);
		wl_list_init(&client->tool->surface_destroy.link);
		client->tool->current_client = NULL;
		client->tool->focused_surface = NULL;
	}

	wl_list_remove(&client->seat_link);
	wl_list_remove(&client->tool_link);
	free(client);

	wl_resource_set_user_data(resource, NULL);
}

static void send_tool_capabilities(struct wl_resource *resource,
		const struct nx_tablet_tool *nx_tool) {
	const struct {
		bool present;
		enum zwp_tablet_tool_v2_capability capability;
	} capabilities[] = {
		{ nx_tool->tilt, ZWP_TABLET_TOOL_V2_CAPABILITY_TILT },
		{ nx_tool->pressure, ZWP_TABLET_TOOL_V2_CAPABILITY_PRESSURE },
		{ nx_tool->distance, ZWP_TABLET_TOOL_V2_CAPABILITY_DISTANCE },
		{ nx_tool->rotation, ZWP_TABLET_TOOL_V2_CAPABILITY_ROTATION },
		{ nx_tool->slider, ZWP_TABLET_TOOL_V2_CAPABILITY_SLIDER },
		{ nx_tool->wheel, ZWP_TABLET_TOOL_V2_CAPABILITY_WHEEL },
	};
	for (size_t i = 0; i < sizeof(capabilities) / sizeof(capabilities[0]); i++) {
		if (capabilities[i].present) {
			zwp_tablet_tool_v2_send_capability(resource,
				capabilities[i].capability);
		}
	}
}

void add_tablet_tool_client(struct nx_tablet_seat_client_v2 *seat,
		struct nx_tablet_v2_tablet_tool *tool) {
	struct nx_tablet_tool_client_v2 *client = calloc(1, sizeof(*client));
	if (!client) {
		return;
	}
	client->tool = tool;
	client->seat = seat;

	uint32_t version = wl_resource_get_version(seat->resource);
	client->resource = wl_resource_create(seat->wl_client,
		&zwp_tablet_tool_v2_interface, version, 0);
	if (!client->resource) {
		free(client);
		return;
	}
	wl_resource_set_implementation(client->resource, &tablet_tool_impl,
		client, destroy_tablet_tool_v2);
	zwp_tablet_seat_v2_send_tool_added(seat->resource, client->resource);

	const struct nx_tablet_tool *nx_tool = tool->nx_tool;
	if (nx_tool->hardware_serial) {
		zwp_tablet_tool_v2_send_hardware_serial(client->resource,
			nx_tool->hardware_serial >> 32,
			nx_tool->hardware_serial & 0xFFFFFFFF);
	}
	if (nx_tool->hardware_wacom) {
		zwp_tablet_tool_v2_send_hardware_id_wacom(client->resource,
			nx_tool->hardware_wacom >> 32,
			nx_tool->hardware_wacom & 0xFFFFFFFF);
	}
	zwp_tablet_tool_v2_send_type(client->resource,
		tablet_type_from_nx_type(nx_tool->type));
	send_tool_capabilities(client->resource, nx_tool);
	zwp_tablet_tool_v2_send_done(client->resource);

	client->client = seat->wl_client;
	wl_list_insert(&seat->tools, &client->seat_link);
	wl_list_insert(&tool->clients, &client->tool_link);
}

static void handle_nx_tablet_tool_destroy(struct wl_listener *listener, void *data) {
	struct nx_tablet_v2_tablet_tool *tool =
		wl_container_of(listener, tool, tool_destroy);

	struct nx_tablet_tool_client_v2 *pos, *next;
	wl_list_for_each_safe(pos, next, &tool->clients, tool_link) {
		zwp_tablet_tool_v2_send_removed(pos->resource);
		pos->tool = NULL;
	}

	wl_list_remove(&tool->clients);
	wl_list_remove(&tool->link);
	wl_list_remove(&tool->tool_destroy.link);
	wl_list_remove(&tool->events.set_cursor.listener_list);
	wl_list_remove(&tool->surface_destroy.link);
	free(tool);
}

static bool tool_type_supported(enum nx_tablet_tool_type type) {
	switch (type) {
	case NX_TABLET_TOOL_TYPE_PEN:
	case NX_TABLET_TOOL_TYPE_ERASER:
	case NX_TABLET_TOOL_TYPE_BRUSH:
	case NX_TABLET_TOOL_TYPE_PENCIL:
	case NX_TABLET_TOOL_TYPE_AIRBRUSH:
	case NX_TABLET_TOOL_TYPE_MOUSE:
	case NX_TABLET_TOOL_TYPE_LENS:
		return true;
	default:
		return false;
	}
}

struct nx_tablet_v2_tablet_tool *nx_tablet_tool_create(
		struct nx_tablet_manager_v2 *manager,
		struct nx_seat *nx_seat,
		struct nx_tablet_tool *nx_tool) {
	if (!tool_type_supported(nx_tool->type)) {
		return NULL;
	}

	struct nx_tablet_seat_v2 *seat = get_or_create_tablet_seat(manager, nx_seat);
	if (!seat) {
		return NULL;
	}
	struct nx_tablet_v2_tablet_tool *tool = calloc(1, sizeof(*tool));
	if (!tool) {
		return NULL;
	}

	tool->nx_tool = nx_tool;
	wl_list_init(&tool->clients);
	wl_list_init(&tool->surface_destroy.link);

	tool->tool_destroy.notify = handle_nx_tablet_tool_destroy;
	wl_signal_add(&nx_tool->events.destroy, &tool->tool_destroy);
	wl_list_insert(&seat->tools, &tool->link);

	struct nx_tablet_seat_client_v2 *pos;
	wl_list_for_each(pos, &seat->clients, seat_link) {
		add_tablet_tool_client(pos, tool);
	}

	wl_signal_init(&tool->events.set_cursor);

	return tool;
}

struct nx_tablet_tool_client_v2 *tablet_tool_client_from_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &zwp_tablet_tool_v2_interface,
		&tablet_tool_impl));
	return wl_resource_get_user_data(resource);
}
