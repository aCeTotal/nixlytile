#include <assert.h>
#include <inttypes.h>
#include <wayland-server-core.h>
#include "platform/clipboard/data_device/data_device.h"
#include "platform/log/log.h"

void data_device_send_selection(struct wl_resource *device_resource) {
	struct nx_seat_client *seat_client =
		seat_client_from_data_device_resource(device_resource);
	assert(seat_client != NULL);

	struct nx_data_source *source = seat_client->seat->selection_source;
	if (source == NULL) {
		wl_data_device_send_selection(device_resource, NULL);
		return;
	}

	struct nx_data_offer *offer = data_offer_create(device_resource,
		source, NX_DATA_OFFER_SELECTION);
	if (offer == NULL) {
		wl_client_post_no_memory(seat_client->client);
		return;
	}
	wl_data_device_send_selection(device_resource, offer->resource);
}

void seat_client_send_selection(struct nx_seat_client *seat_client) {
	struct nx_data_source *source = seat_client->seat->selection_source;
	if (source != NULL) {
		source->accepted = false;
	}

	// Old offers go inert.
	struct nx_data_offer *offer, *tmp;
	wl_list_for_each_safe(offer, tmp,
			&seat_client->seat->selection_offers, link) {
		data_offer_destroy(offer);
	}

	struct wl_resource *device_resource;
	wl_resource_for_each(device_resource, &seat_client->data_devices) {
		data_device_send_selection(device_resource);
	}
}

void nx_seat_request_set_selection(struct nx_seat *seat,
		struct nx_seat_client *client,
		struct nx_data_source *source, uint32_t serial) {
	if (client && !nx_seat_client_validate_event_serial(client, serial)) {
		nx_log(NX_DEBUG, "Rejecting set_selection request, "
			"serial %"PRIu32" was never given to client", serial);
		return;
	}

	if (seat->selection_source &&
			serial - seat->selection_serial > UINT32_MAX / 2) {
		nx_log(NX_DEBUG, "Rejecting set_selection request, serial indicates superseded "
			"(%"PRIu32" < %"PRIu32")", serial, seat->selection_serial);
		return;
	}

	struct nx_seat_request_set_selection_event event = {
		.source = source,
		.serial = serial,
	};
	wl_signal_emit_mutable(&seat->events.request_set_selection, &event);
}

static void seat_announce_selection(struct nx_seat *seat) {
	struct nx_seat_client *focused_client =
		seat->keyboard_state.focused_client;
	if (focused_client != NULL) {
		seat_client_send_selection(focused_client);
	}
	wl_signal_emit_mutable(&seat->events.set_selection, seat);
}

static void seat_handle_selection_source_destroy(
		struct wl_listener *listener, void *data) {
	struct nx_seat *seat =
		wl_container_of(listener, seat, selection_source_destroy);
	wl_list_remove(&seat->selection_source_destroy.link);
	seat->selection_source = NULL;
	seat_announce_selection(seat);
}

void nx_seat_set_selection(struct nx_seat *seat,
		struct nx_data_source *source, uint32_t serial) {
	if (seat->selection_source == source) {
		seat->selection_serial = serial;
		return;
	}

	if (seat->selection_source != NULL) {
		wl_list_remove(&seat->selection_source_destroy.link);
		nx_data_source_destroy(seat->selection_source);
	}

	seat->selection_source = source;
	seat->selection_serial = serial;

	if (source != NULL) {
		seat->selection_source_destroy.notify =
			seat_handle_selection_source_destroy;
		wl_signal_add(&source->events.destroy,
			&seat->selection_source_destroy);
	}

	seat_announce_selection(seat);
}
