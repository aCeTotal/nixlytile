#include <math.h>
#include <wayland-server-core.h>
#include "platform/input/seat/client.h"
#include "platform/surface/compositor.h"

static void handle_surface_destroy(struct wl_listener *listener, void *data) {
	struct nx_seat_pointer_state *state =
		wl_container_of(listener, state, surface_destroy);
	wl_list_remove(&state->surface_destroy.link);
	wl_list_init(&state->surface_destroy.link);
	nx_seat_pointer_clear_focus(state->seat);
}

static void watch_surface(struct nx_seat_pointer_state *state,
		struct nx_surface *surface) {
	wl_list_remove(&state->surface_destroy.link);
	wl_list_init(&state->surface_destroy.link);
	if (surface == NULL) {
		return;
	}
	wl_signal_add(&surface->events.destroy, &state->surface_destroy);
	state->surface_destroy.notify = handle_surface_destroy;
}

void seat_client_send_pointer_leave_raw(struct nx_seat_client *seat_client,
		struct nx_surface *surface) {
	uint32_t serial = nx_seat_client_next_serial(seat_client);
	struct wl_resource *resource;
	wl_resource_for_each(resource, &seat_client->pointers) {
		if (nx_seat_client_from_pointer_resource(resource) == NULL) {
			continue;
		}
		wl_pointer_send_leave(resource, serial, surface->resource);
		seat_pointer_resource_send_frame(resource);
	}
}

// Enters the focused surface.
static void send_enter(const struct nx_seat_pointer_state *state) {
	struct nx_seat_client *client = state->focused_client;
	uint32_t serial = nx_seat_client_next_serial(client);
	struct wl_resource *resource;
	wl_resource_for_each(resource, &client->pointers) {
		if (nx_seat_client_from_pointer_resource(resource) == NULL) {
			continue;
		}
		wl_pointer_send_enter(resource, serial,
			state->focused_surface->resource,
			wl_fixed_from_double(state->sx), wl_fixed_from_double(state->sy));
		seat_pointer_resource_send_frame(resource);
	}
}

void nx_seat_pointer_enter(struct nx_seat *seat,
		struct nx_surface *surface, double sx, double sy) {
	struct nx_seat_pointer_state *state = &seat->pointer_state;
	if (state->focused_surface == surface) {
		return;
	}

	struct nx_seat_client *client = NULL;
	if (surface != NULL) {
		client = nx_seat_client_for_wl_client(seat,
			wl_resource_get_client(surface->resource));
	}
	if (state->focused_client != NULL && state->focused_surface != NULL) {
		seat_client_send_pointer_leave_raw(state->focused_client,
			state->focused_surface);
	}

	watch_surface(state, surface);
	state->focused_client = client;
	state->focused_surface = surface;
	nx_seat_pointer_warp(seat, surface != NULL ? sx : NAN,
		surface != NULL ? sy : NAN);
	if (client != NULL) {
		send_enter(state);
	}
}

void nx_seat_pointer_clear_focus(struct nx_seat *seat) {
	nx_seat_pointer_enter(seat, NULL, 0, 0);
}

void nx_seat_pointer_warp(struct nx_seat *seat, double sx, double sy) {
	seat->pointer_state.sx = sx;
	seat->pointer_state.sy = sy;
}
