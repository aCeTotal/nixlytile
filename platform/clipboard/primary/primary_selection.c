#include <assert.h>
#include <inttypes.h>
#include <stdlib.h>
#include "platform/clipboard/mime_types.h"
#include "platform/clipboard/primary/primary_selection.h"
#include "platform/log/log.h"

void nx_primary_selection_source_init(
		struct nx_primary_selection_source *source,
		const struct nx_primary_selection_source_impl *impl) {
	assert(impl->send);
	*source = (struct nx_primary_selection_source){
		.impl = impl,
	};
	wl_array_init(&source->mime_types);

	wl_signal_init(&source->events.destroy);
}

void nx_primary_selection_source_destroy(
		struct nx_primary_selection_source *source) {
	if (source == NULL) {
		return;
	}

	wl_signal_emit_mutable(&source->events.destroy, source);

	assert(wl_list_empty(&source->events.destroy.listener_list));

	mime_types_release(&source->mime_types);

	if (source->impl->destroy) {
		source->impl->destroy(source);
		return;
	}
	free(source);
}

void nx_primary_selection_source_send(
		struct nx_primary_selection_source *source, const char *mime_type,
		int32_t fd) {
	source->impl->send(source, mime_type, fd);
}

void nx_seat_request_set_primary_selection(struct nx_seat *seat,
		struct nx_seat_client *client,
		struct nx_primary_selection_source *source, uint32_t serial) {
	if (client && !nx_seat_client_validate_event_serial(client, serial)) {
		nx_log(NX_DEBUG, "Rejecting set_primary_selection request, "
			"serial %"PRIu32" was never given to client", serial);
		return;
	}

	if (seat->primary_selection_source &&
			serial - seat->primary_selection_serial > UINT32_MAX / 2) {
		nx_log(NX_DEBUG, "Rejecting set_primary_selection request, "
			"serial indicates superseded (%"PRIu32" < %"PRIu32")",
			serial, seat->primary_selection_serial);
		return;
	}

	struct nx_seat_request_set_primary_selection_event event = {
		.source = source,
		.serial = serial,
	};
	wl_signal_emit_mutable(&seat->events.request_set_primary_selection, &event);
}

static void seat_handle_primary_selection_source_destroy(
		struct wl_listener *listener, void *data) {
	struct nx_seat *seat =
		wl_container_of(listener, seat, primary_selection_source_destroy);
	wl_list_remove(&seat->primary_selection_source_destroy.link);
	seat->primary_selection_source = NULL;
	wl_signal_emit_mutable(&seat->events.set_primary_selection, seat);
}

void nx_seat_set_primary_selection(struct nx_seat *seat,
		struct nx_primary_selection_source *source, uint32_t serial) {
	if (seat->primary_selection_source == source) {
		seat->primary_selection_serial = serial;
		return;
	}

	if (seat->primary_selection_source != NULL) {
		wl_list_remove(&seat->primary_selection_source_destroy.link);
		nx_primary_selection_source_destroy(seat->primary_selection_source);
	}

	seat->primary_selection_source = source;
	seat->primary_selection_serial = serial;

	if (source != NULL) {
		seat->primary_selection_source_destroy.notify =
			seat_handle_primary_selection_source_destroy;
		wl_signal_add(&source->events.destroy,
			&seat->primary_selection_source_destroy);
	}

	wl_signal_emit_mutable(&seat->events.set_primary_selection, seat);
}
