#include <sys/types.h>
#include "platform/container/set.h"
#include "platform/input/tablet/tablet_v2.h"
#include "platform/log/log.h"
#include "platform/os/clock.h"
#include "platform/surface/compositor.h"

#define AXIS_WIRE_MAX 65535

// Index of the button, or -1.
static ssize_t tablet_tool_button_update(struct nx_tablet_v2_tablet_tool *tool,
		uint32_t button, enum zwp_tablet_pad_v2_button_state state) {
	if (state == ZWP_TABLET_PAD_V2_BUTTON_STATE_PRESSED) {
		ssize_t i = set_add(tool->pressed_buttons, &tool->num_buttons,
			NX_TABLET_V2_TOOL_BUTTONS_CAP, button);
		if (i == -1) {
			nx_log(NX_ERROR, "Failed to add tablet tool button %x", button);
			return i;
		}
		tool->pressed_serials[i] = -1;
		return i;
	}

	ssize_t i = set_remove(tool->pressed_buttons, &tool->num_buttons,
		NX_TABLET_V2_TOOL_BUTTONS_CAP, button);
	if (i == -1) {
		nx_log(NX_ERROR, "Failed to remove tablet tool button %x", button);
		return i;
	}
	tool->pressed_serials[i] = tool->pressed_serials[tool->num_buttons];
	return i;
}

static void send_tool_frame(void *data) {
	struct nx_tablet_tool_client_v2 *tool = data;

	zwp_tablet_tool_v2_send_frame(tool->resource, get_current_time_msec());
	tool->frame_source = NULL;
}

static void queue_tool_frame(struct nx_tablet_tool_client_v2 *tool) {
	if (tool->frame_source) {
		return;
	}
	struct wl_display *display = wl_client_get_display(tool->client);
	struct wl_event_loop *loop = wl_display_get_event_loop(display);
	tool->frame_source = wl_event_loop_add_idle(loop, send_tool_frame, tool);
}

static void handle_tablet_tool_surface_destroy(struct wl_listener *listener,
		void *data) {
	struct nx_tablet_v2_tablet_tool *tool =
		wl_container_of(listener, tool, surface_destroy);
	nx_send_tablet_v2_tablet_tool_proximity_out(tool);
}

static struct nx_tablet_client_v2 *find_tablet_client(
		struct nx_tablet_v2_tablet *tablet, struct wl_client *client) {
	struct nx_tablet_client_v2 *tablet_client;
	wl_list_for_each(tablet_client, &tablet->clients, tablet_link) {
		if (tablet_client->client == client) {
			return tablet_client;
		}
	}
	return NULL;
}

static struct nx_tablet_tool_client_v2 *find_tool_client(
		struct nx_tablet_v2_tablet_tool *tool, struct wl_client *client) {
	struct nx_tablet_tool_client_v2 *tool_client;
	wl_list_for_each(tool_client, &tool->clients, tool_link) {
		if (tool_client->client == client) {
			return tool_client;
		}
	}
	return NULL;
}

void nx_send_tablet_v2_tablet_tool_proximity_in(
		struct nx_tablet_v2_tablet_tool *tool,
		struct nx_tablet_v2_tablet *tablet,
		struct nx_surface *surface) {
	struct wl_client *client = wl_resource_get_client(surface->resource);

	if (tool->focused_surface == surface) {
		return;
	}

	nx_send_tablet_v2_tablet_tool_proximity_out(tool);

	// Client needs both bindings.
	struct nx_tablet_client_v2 *tablet_client = find_tablet_client(tablet, client);
	if (!tablet_client) {
		return;
	}
	struct nx_tablet_tool_client_v2 *tool_client = find_tool_client(tool, client);
	if (!tool_client) {
		return;
	}

	wl_list_remove(&tool->surface_destroy.link);
	wl_signal_add(&surface->events.destroy, &tool->surface_destroy);
	tool->surface_destroy.notify = handle_tablet_tool_surface_destroy;

	tool->current_client = tool_client;

	uint32_t serial = nx_seat_client_next_serial(tool_client->seat->seat_client);
	tool->focused_surface = surface;

	zwp_tablet_tool_v2_send_proximity_in(tool_client->resource, serial,
		tablet_client->resource, surface->resource);
	for (size_t i = 0; i < tool->num_buttons; ++i) {
		nx_send_tablet_v2_tablet_tool_button(tool,
			tool->pressed_buttons[i],
			ZWP_TABLET_PAD_V2_BUTTON_STATE_PRESSED);
	}
	if (tool->is_down) {
		nx_send_tablet_v2_tablet_tool_down(tool);
	}

	queue_tool_frame(tool_client);
}

void nx_send_tablet_v2_tablet_tool_motion(
		struct nx_tablet_v2_tablet_tool *tool, double x, double y) {
	if (!tool->current_client) {
		return;
	}

	zwp_tablet_tool_v2_send_motion(tool->current_client->resource,
		wl_fixed_from_double(x), wl_fixed_from_double(y));

	queue_tool_frame(tool->current_client);
}

void nx_send_tablet_v2_tablet_tool_proximity_out(
		struct nx_tablet_v2_tablet_tool *tool) {
	struct nx_tablet_tool_client_v2 *client = tool->current_client;
	if (!client) {
		return;
	}

	for (size_t i = 0; i < tool->num_buttons; ++i) {
		zwp_tablet_tool_v2_send_button(client->resource,
			tool->pressed_serials[i],
			tool->pressed_buttons[i],
			ZWP_TABLET_PAD_V2_BUTTON_STATE_RELEASED);
	}
	if (tool->is_down) {
		zwp_tablet_tool_v2_send_up(client->resource);
	}
	if (client->frame_source) {
		wl_event_source_remove(client->frame_source);
		send_tool_frame(client);
	}
	zwp_tablet_tool_v2_send_proximity_out(client->resource);
	send_tool_frame(client);

	wl_list_remove(&tool->surface_destroy.link);
	wl_list_init(&tool->surface_destroy.link);
	tool->current_client = NULL;
	tool->focused_surface = NULL;
}

void nx_send_tablet_v2_tablet_tool_pressure(
		struct nx_tablet_v2_tablet_tool *tool, double pressure) {
	if (!tool->current_client) {
		return;
	}

	zwp_tablet_tool_v2_send_pressure(tool->current_client->resource,
		(uint32_t)(pressure * AXIS_WIRE_MAX));

	queue_tool_frame(tool->current_client);
}

void nx_send_tablet_v2_tablet_tool_distance(
		struct nx_tablet_v2_tablet_tool *tool, double distance) {
	if (!tool->current_client) {
		return;
	}

	zwp_tablet_tool_v2_send_distance(tool->current_client->resource,
		(uint32_t)(distance * AXIS_WIRE_MAX));

	queue_tool_frame(tool->current_client);
}

void nx_send_tablet_v2_tablet_tool_tilt(
		struct nx_tablet_v2_tablet_tool *tool, double x, double y) {
	if (!tool->current_client) {
		return;
	}

	zwp_tablet_tool_v2_send_tilt(tool->current_client->resource,
		wl_fixed_from_double(x), wl_fixed_from_double(y));

	queue_tool_frame(tool->current_client);
}

void nx_send_tablet_v2_tablet_tool_rotation(
		struct nx_tablet_v2_tablet_tool *tool, double degrees) {
	if (!tool->current_client) {
		return;
	}

	zwp_tablet_tool_v2_send_rotation(tool->current_client->resource,
		wl_fixed_from_double(degrees));

	queue_tool_frame(tool->current_client);
}

void nx_send_tablet_v2_tablet_tool_slider(
		struct nx_tablet_v2_tablet_tool *tool, double position) {
	if (!tool->current_client) {
		return;
	}

	zwp_tablet_tool_v2_send_slider(tool->current_client->resource,
		(int32_t)(position * AXIS_WIRE_MAX));

	queue_tool_frame(tool->current_client);
}

void nx_send_tablet_v2_tablet_tool_button(
		struct nx_tablet_v2_tablet_tool *tool, uint32_t button,
		enum zwp_tablet_pad_v2_button_state state) {
	ssize_t index = tablet_tool_button_update(tool, button, state);
	if (!tool->current_client) {
		return;
	}

	uint32_t serial = nx_seat_client_next_serial(
		tool->current_client->seat->seat_client);
	if (index >= 0) {
		tool->pressed_serials[index] = serial;
	}

	zwp_tablet_tool_v2_send_button(tool->current_client->resource,
		serial, button, state);
	queue_tool_frame(tool->current_client);
}

void nx_send_tablet_v2_tablet_tool_wheel(
		struct nx_tablet_v2_tablet_tool *tool, double degrees, int32_t clicks) {
	if (!tool->current_client) {
		return;
	}

	zwp_tablet_tool_v2_send_wheel(tool->current_client->resource,
		wl_fixed_from_double(degrees), clicks);

	queue_tool_frame(tool->current_client);
}

void nx_send_tablet_v2_tablet_tool_down(struct nx_tablet_v2_tablet_tool *tool) {
	if (tool->is_down) {
		return;
	}

	tool->is_down = true;
	if (!tool->current_client) {
		return;
	}

	uint32_t serial = nx_seat_client_next_serial(
		tool->current_client->seat->seat_client);
	zwp_tablet_tool_v2_send_down(tool->current_client->resource, serial);
	queue_tool_frame(tool->current_client);
}

void nx_send_tablet_v2_tablet_tool_up(struct nx_tablet_v2_tablet_tool *tool) {
	if (!tool->is_down) {
		return;
	}
	tool->is_down = false;

	if (!tool->current_client) {
		return;
	}

	zwp_tablet_tool_v2_send_up(tool->current_client->resource);
	queue_tool_frame(tool->current_client);
}
