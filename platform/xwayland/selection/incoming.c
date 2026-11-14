#include <assert.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include "platform/log/log.h"
#include "platform/xwayland/wm/xwm.h"

#define MAX_PROPERTY_LENGTH 0x1fffffff // in 32-bit units

static struct nx_xwm_selection_transfer *create_incoming(
		struct nx_xwm_selection *selection) {
	struct nx_xwm_selection_transfer *transfer = calloc(1, sizeof(*transfer));
	if (!transfer) {
		return NULL;
	}

	xwm_selection_transfer_init(transfer, selection);
	wl_list_insert(&selection->incoming, &transfer->link);

	struct nx_xwm *xwm = selection->xwm;
	transfer->incoming_window = xcb_generate_id(xwm->xcb_conn);
	xcb_create_window(xwm->xcb_conn, XCB_COPY_FROM_PARENT,
		transfer->incoming_window, xwm->screen->root, 0, 0, 10, 10, 0,
		XCB_WINDOW_CLASS_INPUT_OUTPUT, xwm->screen->root_visual,
		XCB_CW_EVENT_MASK, (uint32_t[]){
			XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY | XCB_EVENT_MASK_PROPERTY_CHANGE
		});
	xwm_schedule_flush(xwm);

	return transfer;
}

struct nx_xwm_selection_transfer *xwm_selection_find_incoming_transfer_by_window(
		struct nx_xwm_selection *selection, xcb_window_t window) {
	struct nx_xwm_selection_transfer *transfer;
	wl_list_for_each(transfer, &selection->incoming, link) {
		if (transfer->incoming_window == window) {
			return transfer;
		}
	}
	return NULL;
}

void xwm_selection_receive(struct nx_xwm_selection *selection,
		xcb_atom_t target, int fd) {
	struct nx_xwm_selection_transfer *transfer = create_incoming(selection);
	if (!transfer) {
		nx_log(NX_ERROR, "Cannot create transfer");
		close(fd);
		return;
	}

	struct nx_xwm *xwm = selection->xwm;
	xcb_convert_selection(xwm->xcb_conn, transfer->incoming_window,
		selection->atom, target, xwm->atoms[WL_SELECTION], XCB_TIME_CURRENT_TIME);
	xwm_schedule_flush(xwm);

	fcntl(fd, F_SETFL, O_WRONLY | O_NONBLOCK);
	transfer->wl_client_fd = fd;
}

static bool fetch_property(struct nx_xwm_selection_transfer *transfer,
		bool delete) {
	struct nx_xwm *xwm = transfer->selection->xwm;
	xcb_get_property_cookie_t cookie = xcb_get_property(xwm->xcb_conn, delete,
		transfer->incoming_window, xwm->atoms[WL_SELECTION],
		XCB_GET_PROPERTY_TYPE_ANY, 0, MAX_PROPERTY_LENGTH);

	transfer->property_start = 0;
	transfer->property_reply =
		xcb_get_property_reply(xwm->xcb_conn, cookie, NULL);
	if (!transfer->property_reply) {
		nx_log(NX_ERROR, "cannot get selection property");
		return false;
	}
	return true;
}

// Deleting property requests next chunk.
static void request_next_chunk(struct nx_xwm_selection_transfer *transfer) {
	struct nx_xwm *xwm = transfer->selection->xwm;
	assert(transfer->incr);

	nx_log(NX_DEBUG, "deleting property");
	xcb_delete_property(xwm->xcb_conn, transfer->incoming_window,
		xwm->atoms[WL_SELECTION]);
	xwm_schedule_flush(xwm);

	xwm_selection_transfer_remove_event_source(transfer);
	xwm_selection_transfer_destroy_property_reply(transfer);
}

// Nonzero while data remains.
static int write_to_client(int fd, uint32_t mask, void *data) {
	struct nx_xwm_selection_transfer *transfer = data;

	const char *property = xcb_get_property_value(transfer->property_reply);
	int length = xcb_get_property_value_length(transfer->property_reply);
	int remainder = length - transfer->property_start;

	ssize_t len = write(fd, property + transfer->property_start, remainder);
	if (len == -1) {
		nx_log_errno(NX_ERROR, "write error to target fd %d", fd);
		xwm_selection_transfer_destroy(transfer);
		return 0;
	}

	nx_log(NX_DEBUG, "wrote %zd (total %zd, remaining %d) of %d bytes to fd %d",
		len, transfer->property_start + len, remainder, length, fd);

	if (len < remainder) {
		transfer->property_start += len;
		return 1;
	}
	if (transfer->incr) {
		request_next_chunk(transfer);
		return 0;
	}
	nx_log(NX_DEBUG, "transfer complete");
	xwm_selection_transfer_destroy(transfer);
	return 0;
}

static void forward_property(struct nx_xwm_selection_transfer *transfer) {
	// Reader gone: keep draining X11.
	if (transfer->incr && transfer->wl_client_fd < 0) {
		request_next_chunk(transfer);
		return;
	}

	if (!write_to_client(transfer->wl_client_fd, WL_EVENT_WRITABLE, transfer)) {
		return;
	}

	// Write the rest when writable.
	struct wl_event_loop *loop = wl_display_get_event_loop(
		transfer->selection->xwm->xwayland->wl_display);
	transfer->event_source = wl_event_loop_add_fd(loop, transfer->wl_client_fd,
		WL_EVENT_WRITABLE, write_to_client, transfer);
}

void xwm_get_incr_chunk(struct nx_xwm_selection_transfer *transfer) {
	nx_log(NX_DEBUG, "xwm_get_incr_chunk");

	if (transfer->property_reply) {
		nx_log(NX_ERROR, "X11 client offered a new property before we deleted");
		return;
	}
	if (!fetch_property(transfer, false)) {
		return;
	}

	if (xcb_get_property_value_length(transfer->property_reply) > 0) {
		forward_property(transfer);
		return;
	}
	nx_log(NX_DEBUG, "incremental transfer complete");
	xwm_selection_transfer_destroy(transfer);
}

static void receive_data(struct nx_xwm_selection_transfer *transfer) {
	struct nx_xwm *xwm = transfer->selection->xwm;
	if (!fetch_property(transfer, true)) {
		return;
	}

	if (transfer->property_reply->type == xwm->atoms[INCR]) {
		transfer->incr = true;
		xwm_selection_transfer_destroy_property_reply(transfer);
		return;
	}
	forward_property(transfer);
}

static void abort_conversion(struct nx_xwm_selection_transfer *transfer) {
	if (transfer == NULL) {
		return;
	}
	nx_log(NX_ERROR, "convert selection failed");
	xwm_selection_transfer_destroy(transfer);
}

void xwm_handle_selection_notify(struct nx_xwm *xwm,
		xcb_selection_notify_event_t *event) {
	nx_log(NX_DEBUG, "XCB_SELECTION_NOTIFY (selection=%u, property=%u, target=%u)",
		event->selection, event->property, event->target);

	struct nx_xwm_selection *selection = xwm_get_selection(xwm, event->selection);
	if (selection == NULL) {
		return;
	}

	struct nx_xwm_selection_transfer *transfer =
		xwm_selection_find_incoming_transfer_by_window(selection, event->requestor);
	if (event->property == XCB_ATOM_NONE) {
		abort_conversion(transfer);
		return;
	}

	// Settable without X11 focus.
	if (event->target == xwm->atoms[TARGETS]) {
		xwm_selection_get_targets(selection);
		return;
	}
	if (transfer) {
		receive_data(transfer);
	}
}
