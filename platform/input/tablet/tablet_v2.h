#ifndef PLATFORM_INPUT_TABLET_TABLET_V2_H
#define PLATFORM_INPUT_TABLET_TABLET_V2_H

#include <wayland-server-core.h>
#include "platform/input/seat/seat.h"
#include "tablet-v2-protocol.h"

#define NX_TABLET_V2_TOOL_BUTTONS_CAP 16

struct nx_input_device;
struct nx_tablet_tool_client_v2;
struct nx_tablet_pad_client_v2;

struct nx_tablet_manager_v2 {
	struct wl_global *wl_global;
	struct wl_list seats; // nx_tablet_seat_v2.link

	struct {
		struct wl_listener display_destroy;
	};
};

struct nx_tablet_v2_tablet {
	struct wl_list link; // nx_tablet_seat_v2.tablets
	struct nx_tablet *nx_tablet;
	struct wl_list clients; // nx_tablet_client_v2.tablet_link

	struct {
		struct wl_listener tablet_destroy;
	};
};

struct nx_tablet_v2_tablet_tool {
	struct wl_list link; // nx_tablet_seat_v2.tools
	struct nx_tablet_tool *nx_tool;
	struct wl_list clients; // nx_tablet_tool_client_v2.tool_link

	struct nx_tablet_tool_client_v2 *current_client;
	struct nx_surface *focused_surface;

	bool is_down;
	size_t num_buttons;
	uint32_t pressed_buttons[NX_TABLET_V2_TOOL_BUTTONS_CAP];
	uint32_t pressed_serials[NX_TABLET_V2_TOOL_BUTTONS_CAP];

	struct {
		struct wl_signal set_cursor; // struct nx_tablet_v2_event_cursor
	} events;

	struct {
		struct wl_listener surface_destroy;
		struct wl_listener tool_destroy;
	};
};

struct nx_tablet_v2_tablet_pad {
	struct wl_list link; // nx_tablet_seat_v2.pads
	struct nx_tablet_pad *nx_pad;
	struct wl_list clients; // nx_tablet_pad_client_v2.pad_link

	size_t group_count;
	uint32_t *groups; // current mode per group

	struct nx_tablet_pad_client_v2 *current_client;

	struct {
		struct wl_listener pad_destroy;
	};
};

struct nx_tablet_v2_event_cursor {
	struct nx_surface *surface;
	uint32_t serial;
	int32_t hotspot_x;
	int32_t hotspot_y;
	struct nx_seat_client *seat_client;
};

struct nx_tablet_v2_tablet *nx_tablet_create(
	struct nx_tablet_manager_v2 *manager,
	struct nx_seat *nx_seat,
	struct nx_input_device *nx_device);

struct nx_tablet_v2_tablet_pad *nx_tablet_pad_create(
	struct nx_tablet_manager_v2 *manager,
	struct nx_seat *nx_seat,
	struct nx_input_device *nx_device);

struct nx_tablet_v2_tablet_tool *nx_tablet_tool_create(
	struct nx_tablet_manager_v2 *manager,
	struct nx_seat *nx_seat,
	struct nx_tablet_tool *nx_tool);

struct nx_tablet_manager_v2 *nx_tablet_v2_create(struct wl_display *display);

void nx_send_tablet_v2_tablet_tool_proximity_in(
	struct nx_tablet_v2_tablet_tool *tool,
	struct nx_tablet_v2_tablet *tablet,
	struct nx_surface *surface);

void nx_send_tablet_v2_tablet_tool_down(struct nx_tablet_v2_tablet_tool *tool);
void nx_send_tablet_v2_tablet_tool_up(struct nx_tablet_v2_tablet_tool *tool);

void nx_send_tablet_v2_tablet_tool_motion(
	struct nx_tablet_v2_tablet_tool *tool, double x, double y);

void nx_send_tablet_v2_tablet_tool_pressure(
	struct nx_tablet_v2_tablet_tool *tool, double pressure);

void nx_send_tablet_v2_tablet_tool_distance(
	struct nx_tablet_v2_tablet_tool *tool, double distance);

void nx_send_tablet_v2_tablet_tool_tilt(
	struct nx_tablet_v2_tablet_tool *tool, double x, double y);

void nx_send_tablet_v2_tablet_tool_rotation(
	struct nx_tablet_v2_tablet_tool *tool, double degrees);

void nx_send_tablet_v2_tablet_tool_slider(
	struct nx_tablet_v2_tablet_tool *tool, double position);

void nx_send_tablet_v2_tablet_tool_wheel(
	struct nx_tablet_v2_tablet_tool *tool, double degrees, int32_t clicks);

void nx_send_tablet_v2_tablet_tool_proximity_out(
	struct nx_tablet_v2_tablet_tool *tool);

void nx_send_tablet_v2_tablet_tool_button(
	struct nx_tablet_v2_tablet_tool *tool, uint32_t button,
	enum zwp_tablet_pad_v2_button_state state);

void nx_send_tablet_v2_tablet_pad_button(
	struct nx_tablet_v2_tablet_pad *pad, size_t button,
	uint32_t time, enum zwp_tablet_pad_v2_button_state state);

void nx_send_tablet_v2_tablet_pad_strip(struct nx_tablet_v2_tablet_pad *pad,
	uint32_t strip, double position, bool finger, uint32_t time);
void nx_send_tablet_v2_tablet_pad_ring(struct nx_tablet_v2_tablet_pad *pad,
	uint32_t ring, double position, bool finger, uint32_t time);

struct nx_tablet_seat_v2 {
	struct wl_list link; // nx_tablet_manager_v2.seats
	struct nx_seat *nx_seat;

	struct wl_list tablets; // nx_tablet_v2_tablet.link
	struct wl_list tools; // nx_tablet_v2_tablet_tool.link
	struct wl_list pads; // nx_tablet_v2_tablet_pad.link

	struct wl_list clients; // nx_tablet_seat_client_v2.seat_link

	struct wl_listener seat_destroy;
};

struct nx_tablet_seat_client_v2 {
	struct wl_list seat_link; // nx_tablet_seat_v2.clients
	struct wl_list client_link; // nx_tablet_manager_client_v2.tablet_seats
	struct wl_client *wl_client;
	struct wl_resource *resource;

	struct nx_seat_client *seat_client;

	struct wl_listener seat_client_destroy;

	struct wl_list tools; // nx_tablet_tool_client_v2.seat_link
	struct wl_list tablets; // nx_tablet_client_v2.seat_link
	struct wl_list pads; // nx_tablet_pad_client_v2.seat_link
};

struct nx_tablet_client_v2 {
	struct wl_list seat_link; // nx_tablet_seat_client_v2.tablets
	struct wl_list tablet_link; // nx_tablet_v2_tablet.clients
	struct wl_client *client;
	struct wl_resource *resource;
};

struct nx_tablet_pad_client_v2 {
	struct wl_list seat_link; // nx_tablet_seat_client_v2.pads
	struct wl_list pad_link; // nx_tablet_v2_tablet_pad.clients
	struct wl_client *client;
	struct wl_resource *resource;
	struct nx_tablet_v2_tablet_pad *pad;
	struct nx_tablet_seat_client_v2 *seat;

	size_t group_count;
	struct wl_resource **groups;

	size_t ring_count;
	struct wl_resource **rings;

	size_t strip_count;
	struct wl_resource **strips;
};

struct nx_tablet_tool_client_v2 {
	struct wl_list seat_link; // nx_tablet_seat_client_v2.tools
	struct wl_list tool_link; // nx_tablet_v2_tablet_tool.clients
	struct wl_client *client;
	struct wl_resource *resource;
	struct nx_tablet_v2_tablet_tool *tool; // NULL once the tool is gone
	struct nx_tablet_seat_client_v2 *seat;

	struct wl_event_source *frame_source;
};

void destroy_tablet_v2(struct wl_resource *resource);
void add_tablet_client(struct nx_tablet_seat_client_v2 *seat, struct nx_tablet_v2_tablet *tablet);

void destroy_tablet_pad_v2(struct wl_resource *resource);
struct nx_tablet_pad_client_v2 *tablet_pad_client_from_resource(struct wl_resource *resource);
void add_tablet_pad_client(struct nx_tablet_seat_client_v2 *seat, struct nx_tablet_v2_tablet_pad *pad);

void destroy_tablet_tool_v2(struct wl_resource *resource);
struct nx_tablet_tool_client_v2 *tablet_tool_client_from_resource(struct wl_resource *resource);
void add_tablet_tool_client(struct nx_tablet_seat_client_v2 *seat, struct nx_tablet_v2_tablet_tool *tool);

void tablet_seat_client_v2_destroy(struct wl_resource *resource);
struct nx_tablet_seat_v2 *get_or_create_tablet_seat(
	struct nx_tablet_manager_v2 *manager,
	struct nx_seat *nx_seat);

#endif
