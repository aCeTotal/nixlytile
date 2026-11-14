#include <stdlib.h>
#include <unistd.h>
#include "platform/xwayland/wm/xwm.h"

// Zeroes the transfer, including source_data.
void xwm_selection_transfer_init(struct nx_xwm_selection_transfer *transfer,
		struct nx_xwm_selection *selection) {
	*transfer = (struct nx_xwm_selection_transfer){
		.selection = selection,
		.wl_client_fd = -1,
	};
}

void xwm_selection_transfer_remove_event_source(
		struct nx_xwm_selection_transfer *transfer) {
	if (transfer->event_source != NULL) {
		wl_event_source_remove(transfer->event_source);
		transfer->event_source = NULL;
	}
}

void xwm_selection_transfer_close_wl_client_fd(
		struct nx_xwm_selection_transfer *transfer) {
	if (transfer->wl_client_fd >= 0) {
		close(transfer->wl_client_fd);
		transfer->wl_client_fd = -1;
	}
}

void xwm_selection_transfer_destroy_property_reply(
		struct nx_xwm_selection_transfer *transfer) {
	free(transfer->property_reply);
	transfer->property_reply = NULL;
}

void xwm_selection_transfer_destroy(struct nx_xwm_selection_transfer *transfer) {
	xwm_selection_transfer_destroy_property_reply(transfer);
	xwm_selection_transfer_remove_event_source(transfer);
	xwm_selection_transfer_close_wl_client_fd(transfer);

	if (transfer->incoming_window) {
		struct nx_xwm *xwm = transfer->selection->xwm;
		xcb_destroy_window(xwm->xcb_conn, transfer->incoming_window);
		xwm_schedule_flush(xwm);
	}

	wl_list_remove(&transfer->link);
	free(transfer);
}
