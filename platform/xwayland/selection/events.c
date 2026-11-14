#include "platform/log/log.h"
#include "platform/xwayland/wm/xwm.h"

struct nx_xwm_selection *xwm_get_selection(struct nx_xwm *xwm,
		xcb_atom_t selection_atom) {
	if (selection_atom == xwm->atoms[CLIPBOARD]) {
		return &xwm->clipboard_selection;
	}
	if (selection_atom == xwm->atoms[PRIMARY]) {
		return &xwm->primary_selection;
	}
	if (selection_atom == xwm->atoms[DND_SELECTION]) {
		return &xwm->dnd_selection;
	}
	return NULL;
}

// Next INCR chunk arrived.
static bool route_incoming(struct nx_xwm_selection *selection,
		const xcb_property_notify_event_t *event) {
	if (event->state != XCB_PROPERTY_NEW_VALUE ||
			event->atom != selection->xwm->atoms[WL_SELECTION]) {
		return false;
	}

	struct nx_xwm_selection_transfer *transfer =
		xwm_selection_find_incoming_transfer_by_window(selection, event->window);
	if (transfer == NULL) {
		return false;
	}
	if (transfer->incr) {
		xwm_get_incr_chunk(transfer);
	}
	return true;
}

// Requestor consumed our INCR chunk.
static bool route_outgoing(struct nx_xwm_selection *selection,
		const xcb_property_notify_event_t *event) {
	struct nx_xwm_selection_transfer *transfer;
	wl_list_for_each(transfer, &selection->outgoing, link) {
		if (event->window != transfer->request.requestor) {
			continue;
		}
		if (event->state == XCB_PROPERTY_DELETE &&
				event->atom == transfer->request.property && transfer->incr) {
			xwm_send_incr_chunk(transfer);
		}
		return true;
	}
	return false;
}

static int handle_property_notify(struct nx_xwm *xwm,
		const xcb_property_notify_event_t *event) {
	struct nx_xwm_selection *selections[] = {
		&xwm->clipboard_selection,
		&xwm->primary_selection,
		&xwm->dnd_selection,
	};

	for (size_t i = 0; i < sizeof(selections) / sizeof(selections[0]); i++) {
		if (route_incoming(selections[i], event) ||
				route_outgoing(selections[i], event)) {
			return 1;
		}
	}
	return 0;
}

int xwm_handle_selection_event(struct nx_xwm *xwm, xcb_generic_event_t *event) {
	if (xwm->seat == NULL) {
		nx_log(NX_DEBUG, "not handling selection events: "
			"no seat assigned to xwayland");
		return 0;
	}

	switch (event->response_type & XCB_EVENT_RESPONSE_TYPE_MASK) {
	case XCB_SELECTION_NOTIFY:
		xwm_handle_selection_notify(xwm, (xcb_selection_notify_event_t *)event);
		return 1;
	case XCB_PROPERTY_NOTIFY:
		return handle_property_notify(xwm, (xcb_property_notify_event_t *)event);
	case XCB_SELECTION_REQUEST:
		xwm_handle_selection_request(xwm, (xcb_selection_request_event_t *)event);
		return 1;
	}

	if (event->response_type - xwm->xfixes->first_event ==
			XCB_XFIXES_SELECTION_NOTIFY) {
		return xwm_handle_xfixes_selection_notify(xwm,
			(xcb_xfixes_selection_notify_event_t *)event);
	}
	return 0;
}
