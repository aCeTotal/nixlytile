#include <wayland-server-core.h>
#include "platform/input/seat/seat.h"

static void ringset_add(struct nx_serial_ringset *set, uint32_t serial) {
	if (set->count == 0) {
		set->data[0].min_incl = serial;
		set->data[0].max_incl = serial;
		set->count = 1;
		set->end = 0;
		return;
	}
	if (set->data[set->end].max_incl + 1 == serial) {
		set->data[set->end].max_incl = serial;
		return;
	}
	if (set->count < NX_SERIAL_RINGSET_SIZE) {
		set->count++;
	}
	set->end = (set->end + 1) % NX_SERIAL_RINGSET_SIZE;
	set->data[set->end].min_incl = serial;
	set->data[set->end].max_incl = serial;
}

uint32_t nx_seat_client_next_serial(struct nx_seat_client *client) {
	struct wl_display *display = wl_client_get_display(client->client);
	uint32_t serial = wl_display_next_serial(display);
	if (serial == 0) {
		serial = wl_display_next_serial(display);
	}
	ringset_add(&client->serials, serial);
	return serial;
}

bool nx_seat_client_validate_event_serial(struct nx_seat_client *client,
		uint32_t serial) {
	uint32_t cur = wl_display_get_serial(wl_client_get_display(client->client));
	struct nx_serial_ringset *set = &client->serials;
	uint32_t rev_dist = cur - serial;

	// Newer than current: invalid.
	if (rev_dist >= UINT32_MAX / 2) {
		return false;
	}

	for (int i = 0; i < set->count; i++) {
		int j = (set->end - i + NX_SERIAL_RINGSET_SIZE) % NX_SERIAL_RINGSET_SIZE;
		if (rev_dist < cur - set->data[j].max_incl) {
			return false;
		}
		if (rev_dist <= cur - set->data[j].min_incl) {
			return true;
		}
	}

	// Full ring may have recycled.
	return set->count == NX_SERIAL_RINGSET_SIZE;
}
