#include <inttypes.h>
#include <stdlib.h>
#include <unistd.h>
#include "platform/log/log.h"
#include "platform/xwayland/wm/xwm.h"

void xwm_selection_send_notify(struct nx_xwm *xwm,
		xcb_selection_request_event_t *req, bool success) {
	xcb_selection_notify_event_t selection_notify = {
		.response_type = XCB_SELECTION_NOTIFY,
		.sequence = 0,
		.time = req->time,
		.requestor = req->requestor,
		.selection = req->selection,
		.target = req->target,
		.property = success ? req->property : XCB_ATOM_NONE,
	};

	nx_log(NX_DEBUG, "SendEvent destination=%" PRIu32 " SelectionNotify(31) time=%" PRIu32
		" requestor=%" PRIu32 " selection=%" PRIu32 " target=%" PRIu32 " property=%" PRIu32,
		req->requestor, req->time, req->requestor, req->selection, req->target,
		selection_notify.property);
	xwm_send_event(xwm, req->requestor, XCB_EVENT_MASK_NO_EVENT,
		&selection_notify, sizeof(selection_notify));
	xwm_schedule_flush(xwm);
}

static size_t flush_source_data(struct nx_xwm_selection_transfer *transfer) {
	xcb_change_property(transfer->selection->xwm->xcb_conn,
		XCB_PROP_MODE_REPLACE, transfer->request.requestor,
		transfer->request.property, transfer->request.target, 8,
		transfer->source_data.size, transfer->source_data.data);
	xwm_schedule_flush(transfer->selection->xwm);
	transfer->property_set = true;
	size_t length = transfer->source_data.size;
	transfer->source_data.size = 0;
	return length;
}

void xwm_selection_transfer_destroy_outgoing(
		struct nx_xwm_selection_transfer *transfer) {
	wl_list_remove(&transfer->link);
	nx_log(NX_DEBUG, "Destroying transfer %p", (void *)transfer);

	xwm_selection_transfer_remove_event_source(transfer);
	xwm_selection_transfer_close_wl_client_fd(transfer);
	wl_array_release(&transfer->source_data);
	free(transfer);
}

static void fail_outgoing(struct nx_xwm_selection_transfer *transfer) {
	xwm_selection_send_notify(transfer->selection->xwm, &transfer->request, false);
	xwm_selection_transfer_destroy_outgoing(transfer);
}

// Large data uses INCR chunks.
static void start_incr(struct nx_xwm_selection_transfer *transfer) {
	struct nx_xwm *xwm = transfer->selection->xwm;
	nx_log(NX_DEBUG, "got %zu bytes, starting incr", transfer->source_data.size);

	uint32_t chunk_size = INCR_CHUNK_SIZE;
	xcb_change_property(xwm->xcb_conn, XCB_PROP_MODE_REPLACE,
		transfer->request.requestor, transfer->request.property,
		xwm->atoms[INCR], 32, 1, &chunk_size);
	transfer->incr = true;
	transfer->property_set = true;
	transfer->flush_property_on_delete = true;
	xwm_selection_transfer_remove_event_source(transfer);
	xwm_selection_send_notify(xwm, &transfer->request, true);
}

static void handle_full_chunk(struct nx_xwm_selection_transfer *transfer) {
	if (!transfer->incr) {
		start_incr(transfer);
		return;
	}
	if (transfer->property_set) {
		nx_log(NX_DEBUG, "got %zu bytes, waiting for property delete",
			transfer->source_data.size);
		transfer->flush_property_on_delete = true;
		xwm_selection_transfer_remove_event_source(transfer);
		return;
	}
	nx_log(NX_DEBUG, "got %zu bytes, property deleted, setting new "
		"property", transfer->source_data.size);
	flush_source_data(transfer);
}

static void finish_incr(struct nx_xwm_selection_transfer *transfer) {
	nx_log(NX_DEBUG, "incr transfer complete");
	transfer->flush_property_on_delete = true;
	if (transfer->property_set) {
		nx_log(NX_DEBUG, "got %zu bytes, waiting for property delete",
			transfer->source_data.size);
	} else {
		nx_log(NX_DEBUG, "got %zu bytes, property deleted, setting new "
			"property", transfer->source_data.size);
		flush_source_data(transfer);
	}
	xwm_selection_transfer_remove_event_source(transfer);
	xwm_selection_transfer_close_wl_client_fd(transfer);
}

static void handle_data(struct nx_xwm_selection_transfer *transfer, ssize_t len) {
	if (transfer->source_data.size >= INCR_CHUNK_SIZE) {
		handle_full_chunk(transfer);
		return;
	}
	if (len > 0) {
		nx_log(NX_DEBUG, "nothing happened, buffered the bytes");
		return;
	}
	if (transfer->incr) {
		finish_incr(transfer);
		return;
	}

	nx_log(NX_DEBUG, "non-incr transfer complete");
	flush_source_data(transfer);
	xwm_selection_send_notify(transfer->selection->xwm, &transfer->request, true);
	xwm_selection_transfer_destroy_outgoing(transfer);
}

// Space for one more chunk.
static void *reserve_chunk(struct wl_array *buffer) {
	if (buffer->size >= INCR_CHUNK_SIZE) {
		return (char *)buffer->data + buffer->size;
	}
	return wl_array_add(buffer, INCR_CHUNK_SIZE);
}

static int read_data_source(int fd, uint32_t mask, void *data) {
	struct nx_xwm_selection_transfer *transfer = data;
	struct wl_array *buffer = &transfer->source_data;

	size_t current = buffer->size;
	void *p = reserve_chunk(buffer);
	if (p == NULL) {
		nx_log(NX_ERROR, "Could not allocate selection source_data");
		fail_outgoing(transfer);
		return 0;
	}

	size_t available = buffer->alloc - current;
	ssize_t len = read(fd, p, available);
	if (len == -1) {
		nx_log_errno(NX_ERROR, "read error from data source");
		fail_outgoing(transfer);
		return 0;
	}

	nx_log(NX_DEBUG, "read %zd bytes (available %zu, mask 0x%x)", len,
		available, mask);
	buffer->size = current + len;
	handle_data(transfer, len);
	return 1;
}

void xwm_selection_transfer_start_outgoing(
		struct nx_xwm_selection_transfer *transfer) {
	struct wl_event_loop *loop = wl_display_get_event_loop(
		transfer->selection->xwm->xwayland->wl_display);
	nx_log(NX_DEBUG, "Starting transfer %p", (void *)transfer);
	transfer->event_source = wl_event_loop_add_fd(loop, transfer->wl_client_fd,
		WL_EVENT_READABLE, read_data_source, transfer);
}

void xwm_send_incr_chunk(struct nx_xwm_selection_transfer *transfer) {
	nx_log(NX_DEBUG, "property deleted");

	transfer->property_set = false;
	if (!transfer->flush_property_on_delete) {
		return;
	}

	nx_log(NX_DEBUG, "setting new property, %zu bytes", transfer->source_data.size);
	transfer->flush_property_on_delete = false;
	size_t length = flush_source_data(transfer);

	if (transfer->wl_client_fd >= 0) {
		xwm_selection_transfer_start_outgoing(transfer);
		return;
	}
	if (length == 0) {
		xwm_selection_transfer_destroy_outgoing(transfer);
		return;
	}

	// An empty chunk ends INCR.
	transfer->flush_property_on_delete = true;
	wl_array_release(&transfer->source_data);
	wl_array_init(&transfer->source_data);
}
