#ifndef PLATFORM_INPUT_SEAT_SEAT_H
#define PLATFORM_INPUT_SEAT_SEAT_H

#include <wayland-server-core.h>
#include "platform/input/seat/keyboard.h"
#include "platform/input/seat/pointer.h"

#define NX_SERIAL_RINGSET_SIZE 128

struct nx_serial_range {
	uint32_t min_incl;
	uint32_t max_incl;
};

struct nx_serial_ringset {
	struct nx_serial_range data[NX_SERIAL_RINGSET_SIZE];
	int end;
	int count;
};

// Client bound to the seat.
struct nx_seat_client {
	struct wl_client *client;
	struct nx_seat *seat;
	struct wl_list link;

	struct wl_list resources;
	struct wl_list pointers;
	struct wl_list keyboards;
	struct wl_list data_devices;

	struct {
		struct wl_signal destroy;
	} events;

	struct nx_serial_ringset serials;

	// Wheel accumulators for low-res clients.
	struct {
		int32_t acc_discrete[2];
		int32_t last_discrete[2];
		double acc_axis[2];
	} value120;
};

struct nx_primary_selection_source;

struct nx_seat {
	struct wl_global *global;
	struct wl_display *display;
	struct wl_list clients;

	char *name;
	uint32_t capabilities;
	uint32_t accumulated_capabilities;

	struct nx_data_source *selection_source;
	uint32_t selection_serial;
	struct wl_list selection_offers; // nx_data_offer.link

	struct nx_primary_selection_source *primary_selection_source;
	uint32_t primary_selection_serial;

	// Drag ends before its source.
	struct nx_drag *drag;
	struct nx_data_source *drag_source;
	uint32_t drag_serial;
	struct wl_list drag_offers; // nx_data_offer.link

	struct nx_seat_pointer_state pointer_state;
	struct nx_seat_keyboard_state keyboard_state;

	struct {
		struct wl_signal request_set_cursor; // nx_seat_pointer_request_set_cursor_event
		struct wl_signal request_set_selection; // nx_seat_request_set_selection_event
		struct wl_signal set_selection;
		struct wl_signal request_set_primary_selection; // nx_seat_request_set_primary_selection_event
		struct wl_signal set_primary_selection;
		struct wl_signal request_start_drag; // nx_seat_request_start_drag_event
		struct wl_signal start_drag; // nx_drag
		struct wl_signal destroy;
	} events;

	struct wl_listener display_destroy;
	struct wl_listener selection_source_destroy;
	struct wl_listener primary_selection_source_destroy;
	struct wl_listener drag_source_destroy;
};

struct nx_seat_request_set_selection_event {
	struct nx_data_source *source;
	uint32_t serial;
};

struct nx_seat_request_set_primary_selection_event {
	struct nx_primary_selection_source *source;
	uint32_t serial;
};

struct nx_seat_request_start_drag_event {
	struct nx_drag *drag;
	struct nx_surface *origin;
	uint32_t serial;
};

struct nx_seat *nx_seat_create(struct wl_display *display, const char *name);
void nx_seat_set_capabilities(struct nx_seat *seat, uint32_t capabilities);

struct nx_seat_client *nx_seat_client_for_wl_client(struct nx_seat *seat,
		struct wl_client *wl_client);
// NULL for inert resources.
struct nx_seat_client *nx_seat_client_from_resource(
		struct wl_resource *resource);

// Serials handed out per client.
uint32_t nx_seat_client_next_serial(struct nx_seat_client *client);
bool nx_seat_client_validate_event_serial(struct nx_seat_client *client,
		uint32_t serial);

#endif
