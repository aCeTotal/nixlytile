#include "platform/clipboard/data_device/data_device.h"
#include "platform/clipboard/primary/primary_selection.h"
#include "platform/input/seat/seat.h"
#include "platform/log/log.h"
#include "platform/xwayland/wm/xwm.h"

#define SELECTION_EVENT_MASK \
	(XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY | XCB_EVENT_MASK_PROPERTY_CHANGE)

// Screen-sized to catch Xdnd messages.
static void create_dnd_window(struct nx_xwm_selection *selection) {
	struct nx_xwm *xwm = selection->xwm;
	xcb_create_window(xwm->xcb_conn, XCB_COPY_FROM_PARENT, selection->window,
		xwm->screen->root, 0, 0, 8192, 8192, 0, XCB_WINDOW_CLASS_INPUT_ONLY,
		xwm->screen->root_visual, XCB_CW_EVENT_MASK,
		(uint32_t[]){ SELECTION_EVENT_MASK });

	xcb_change_property(xwm->xcb_conn, XCB_PROP_MODE_REPLACE,
		selection->window, xwm->atoms[DND_AWARE], XCB_ATOM_ATOM, 32, 1,
		&(uint32_t){ XDND_VERSION });
}

static void create_proxy_window(struct nx_xwm_selection *selection) {
	struct nx_xwm *xwm = selection->xwm;
	xcb_create_window(xwm->xcb_conn, XCB_COPY_FROM_PARENT, selection->window,
		xwm->screen->root, 0, 0, 10, 10, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
		xwm->screen->root_visual, XCB_CW_EVENT_MASK,
		(uint32_t[]){ SELECTION_EVENT_MASK });

	if (selection->atom == xwm->atoms[CLIPBOARD]) {
		xcb_set_selection_owner(xwm->xcb_conn, selection->window,
			xwm->atoms[CLIPBOARD_MANAGER], XCB_TIME_CURRENT_TIME);
	}
}

void xwm_selection_init(struct nx_xwm_selection *selection,
		struct nx_xwm *xwm, xcb_atom_t atom) {
	*selection = (struct nx_xwm_selection){
		.xwm = xwm,
		.atom = atom,
		.window = xcb_generate_id(xwm->xcb_conn),
	};
	wl_list_init(&selection->incoming);
	wl_list_init(&selection->outgoing);

	if (atom == xwm->atoms[DND_SELECTION]) {
		create_dnd_window(selection);
	} else {
		create_proxy_window(selection);
	}

	uint32_t mask = XCB_XFIXES_SELECTION_EVENT_MASK_SET_SELECTION_OWNER |
		XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_WINDOW_DESTROY |
		XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_CLIENT_CLOSE;
	xcb_xfixes_select_selection_input(xwm->xcb_conn, selection->window,
		selection->atom, mask);
}

void xwm_selection_finish(struct nx_xwm_selection *selection) {
	struct nx_xwm_selection_transfer *transfer, *tmp;
	wl_list_for_each_safe(transfer, tmp, &selection->outgoing, link) {
		nx_log(NX_INFO, "destroyed pending transfer %p", (void *)transfer);
		xwm_selection_transfer_destroy_outgoing(transfer);
	}
	wl_list_for_each_safe(transfer, tmp, &selection->incoming, link) {
		xwm_selection_transfer_destroy(transfer);
	}

	xcb_destroy_window(selection->xwm->xcb_conn, selection->window);
}

static void claim_selection(struct nx_xwm_selection *selection) {
	xcb_set_selection_owner(selection->xwm->xcb_conn, selection->window,
		selection->atom, XCB_TIME_CURRENT_TIME);
	xwm_schedule_flush(selection->xwm);
}

static void release_selection(struct nx_xwm_selection *selection) {
	if (selection->owner != selection->window) {
		return;
	}
	xcb_set_selection_owner(selection->xwm->xcb_conn, XCB_WINDOW_NONE,
		selection->atom, selection->timestamp);
	xwm_schedule_flush(selection->xwm);
}

static void handle_seat_set_selection(struct wl_listener *listener,
		void *data) {
	struct nx_seat *seat = data;
	struct nx_xwm *xwm = wl_container_of(listener, xwm, seat_set_selection);
	struct nx_data_source *source = seat->selection_source;

	if (source == NULL) {
		release_selection(&xwm->clipboard_selection);
		return;
	}
	if (!xwm_data_source_is_xwayland(source)) {
		claim_selection(&xwm->clipboard_selection);
	}
}

static void handle_seat_set_primary_selection(struct wl_listener *listener,
		void *data) {
	struct nx_seat *seat = data;
	struct nx_xwm *xwm =
		wl_container_of(listener, xwm, seat_set_primary_selection);
	struct nx_primary_selection_source *source = seat->primary_selection_source;

	if (source == NULL) {
		release_selection(&xwm->primary_selection);
		return;
	}
	if (!xwm_primary_selection_source_is_xwayland(source)) {
		claim_selection(&xwm->primary_selection);
	}
}

static void handle_seat_start_drag(struct wl_listener *listener, void *data) {
	struct nx_xwm *xwm = wl_container_of(listener, xwm, seat_start_drag);
	struct nx_drag *drag = data;

	if (drag != NULL) {
		claim_selection(&xwm->dnd_selection);
	} else {
		release_selection(&xwm->dnd_selection);
	}
	xwm_seat_handle_start_drag(xwm, drag);
}

void xwm_set_seat(struct nx_xwm *xwm, struct nx_seat *seat) {
	if (xwm->seat != NULL) {
		wl_list_remove(&xwm->seat_set_selection.link);
		wl_list_remove(&xwm->seat_set_primary_selection.link);
		wl_list_remove(&xwm->seat_start_drag.link);
		xwm->seat = NULL;
	}
	if (seat == NULL) {
		return;
	}

	xwm->seat = seat;
	xwm->seat_set_selection.notify = handle_seat_set_selection;
	wl_signal_add(&seat->events.set_selection, &xwm->seat_set_selection);
	xwm->seat_set_primary_selection.notify = handle_seat_set_primary_selection;
	wl_signal_add(&seat->events.set_primary_selection,
		&xwm->seat_set_primary_selection);
	xwm->seat_start_drag.notify = handle_seat_start_drag;
	wl_signal_add(&seat->events.start_drag, &xwm->seat_start_drag);

	handle_seat_set_selection(&xwm->seat_set_selection, seat);
	handle_seat_set_primary_selection(&xwm->seat_set_primary_selection, seat);
}

static void clear_wayland_selection(struct nx_xwm_selection *selection) {
	struct nx_xwm *xwm = selection->xwm;
	struct wl_display *display = xwm->xwayland->wl_display;
	if (selection == &xwm->clipboard_selection) {
		nx_seat_request_set_selection(xwm->seat, NULL, NULL,
			wl_display_next_serial(display));
		return;
	}
	if (selection == &xwm->primary_selection) {
		nx_seat_request_set_primary_selection(xwm->seat, NULL, NULL,
			wl_display_next_serial(display));
	}
}

// A real X11 owner left.
static void handle_owner_gone(struct nx_xwm_selection *selection) {
	if (selection->owner != selection->window) {
		clear_wayland_selection(selection);
	}
	selection->owner = XCB_WINDOW_NONE;
}

int xwm_handle_xfixes_selection_notify(struct nx_xwm *xwm,
		xcb_xfixes_selection_notify_event_t *event) {
	nx_log(NX_DEBUG, "XCB_XFIXES_SELECTION_NOTIFY (selection=%u, owner=%u)",
		event->selection, event->owner);

	struct nx_xwm_selection *selection = xwm_get_selection(xwm, event->selection);
	if (selection == NULL) {
		return 0;
	}

	if (event->owner == XCB_WINDOW_NONE) {
		handle_owner_gone(selection);
		return 1;
	}

	if (selection->owner == selection->window && event->owner != selection->owner) {
		nx_log(NX_DEBUG, "proxy window lost selection ownership");
	}
	selection->owner = event->owner;

	// Keep the real claim time.
	if (selection->owner == selection->window) {
		selection->timestamp = event->timestamp;
		return 1;
	}

	// SelectionNotify finishes the sync.
	xcb_convert_selection(xwm->xcb_conn, selection->window, selection->atom,
		xwm->atoms[TARGETS], xwm->atoms[WL_SELECTION], event->timestamp);
	xwm_schedule_flush(xwm);
	return 1;
}
