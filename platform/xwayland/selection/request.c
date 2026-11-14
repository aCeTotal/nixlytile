#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "platform/clipboard/data_device/data_device.h"
#include "platform/clipboard/primary/primary_selection.h"
#include "platform/input/seat/seat.h"
#include "platform/log/log.h"
#include "platform/xwayland/wm/xwm.h"

// Clipboard or drag source.
static struct nx_data_source *data_source(struct nx_xwm_selection *selection) {
	struct nx_xwm *xwm = selection->xwm;
	if (selection == &xwm->clipboard_selection) {
		return xwm->seat->selection_source;
	}
	if (selection == &xwm->dnd_selection) {
		return xwm->seat->drag_source;
	}
	return NULL;
}

static struct nx_primary_selection_source *primary_source(
		struct nx_xwm_selection *selection) {
	struct nx_xwm *xwm = selection->xwm;
	if (selection != &xwm->primary_selection) {
		return NULL;
	}
	return xwm->seat->primary_selection_source;
}

static void source_send(struct nx_xwm_selection *selection,
		const char *mime_type, int fd) {
	struct nx_data_source *source = data_source(selection);
	if (source != NULL) {
		nx_data_source_send(source, mime_type, fd);
		return;
	}
	struct nx_primary_selection_source *primary = primary_source(selection);
	if (primary != NULL) {
		nx_primary_selection_source_send(primary, mime_type, fd);
		return;
	}
	nx_log(NX_DEBUG, "not sending selection: no selection source available");
}

static struct wl_array *source_mime_types(struct nx_xwm_selection *selection) {
	struct nx_data_source *source = data_source(selection);
	if (source != NULL) {
		return &source->mime_types;
	}
	struct nx_primary_selection_source *primary = primary_source(selection);
	if (primary != NULL) {
		return &primary->mime_types;
	}
	return NULL;
}

static bool has_mime_type(struct wl_array *mime_types, const char *mime_type) {
	char **offered;
	wl_array_for_each(offered, mime_types) {
		if (strcmp(*offered, mime_type) == 0) {
			return true;
		}
	}
	return false;
}

// Requestors only read their latest request.
static void drop_stale_transfers(struct nx_xwm_selection *selection,
		xcb_window_t requestor) {
	struct nx_xwm_selection_transfer *outgoing, *tmp;
	wl_list_for_each_safe(outgoing, tmp, &selection->outgoing, link) {
		if (outgoing->request.requestor != requestor) {
			nx_log(NX_DEBUG, "Transfer %p still running", (void *)outgoing);
			continue;
		}
		nx_log(NX_DEBUG, "Destroying stale transfer %p", (void *)outgoing);
		xwm_selection_send_notify(selection->xwm, &outgoing->request, false);
		xwm_selection_transfer_destroy_outgoing(outgoing);
	}
}

static bool send_data(struct nx_xwm_selection *selection,
		xcb_selection_request_event_t *req, const char *mime_type) {
	struct wl_array *mime_types = source_mime_types(selection);
	if (mime_types == NULL) {
		nx_log(NX_ERROR, "not sending selection: no MIME type list available");
		return false;
	}
	if (!has_mime_type(mime_types, mime_type)) {
		nx_log(NX_ERROR, "not sending selection: "
			"requested an unsupported MIME type %s", mime_type);
		return false;
	}

	struct nx_xwm_selection_transfer *transfer = calloc(1, sizeof(*transfer));
	if (transfer == NULL) {
		nx_log(NX_ERROR, "Allocation failed");
		return false;
	}
	xwm_selection_transfer_init(transfer, selection);
	transfer->request = *req;

	int p[2];
	if (pipe(p) == -1) {
		nx_log_errno(NX_ERROR, "pipe() failed");
		free(transfer);
		return false;
	}
	fcntl(p[0], F_SETFD, FD_CLOEXEC);
	fcntl(p[0], F_SETFL, O_NONBLOCK);
	fcntl(p[1], F_SETFD, FD_CLOEXEC);
	fcntl(p[1], F_SETFL, O_NONBLOCK);
	transfer->wl_client_fd = p[0];

	nx_log(NX_DEBUG, "Sending Wayland selection %u to Xwayland window with "
		"MIME type %s, target %u, transfer %p", req->target, mime_type,
		req->target, (void *)transfer);
	source_send(selection, mime_type, p[1]);

	drop_stale_transfers(selection, transfer->request.requestor);
	wl_list_insert(&selection->outgoing, &transfer->link);
	xwm_selection_transfer_start_outgoing(transfer);
	return true;
}

static void send_targets(struct nx_xwm_selection *selection,
		xcb_selection_request_event_t *req) {
	struct nx_xwm *xwm = selection->xwm;
	struct wl_array *mime_types = source_mime_types(selection);
	if (mime_types == NULL) {
		nx_log(NX_ERROR, "not sending selection targets: "
			"no selection source available");
		xwm_selection_send_notify(xwm, req, false);
		return;
	}

	size_t n = 2 + mime_types->size / sizeof(char *);
	xcb_atom_t *targets = malloc(n * sizeof(targets[0]));
	if (targets == NULL) {
		nx_log(NX_ERROR, "Allocation failure");
		return;
	}
	targets[0] = xwm->atoms[TIMESTAMP];
	targets[1] = xwm->atoms[TARGETS];

	size_t i = 2;
	char **mime_type;
	wl_array_for_each(mime_type, mime_types) {
		targets[i++] = xwm_mime_type_to_atom(xwm, *mime_type);
	}

	xcb_change_property(xwm->xcb_conn, XCB_PROP_MODE_REPLACE, req->requestor,
		req->property, XCB_ATOM_ATOM, 32, n, targets);
	free(targets);

	xwm_selection_send_notify(xwm, req, true);
}

static void send_timestamp(struct nx_xwm_selection *selection,
		xcb_selection_request_event_t *req) {
	xcb_change_property(selection->xwm->xcb_conn, XCB_PROP_MODE_REPLACE,
		req->requestor, req->property, XCB_ATOM_INTEGER, 32, 1,
		&selection->timestamp);
	xwm_selection_send_notify(selection->xwm, req, true);
}

// False if requestor gets nothing.
static bool answer_request(struct nx_xwm_selection *selection,
		xcb_selection_request_event_t *req) {
	struct nx_xwm *xwm = selection->xwm;
	if (req->target == xwm->atoms[TARGETS]) {
		send_targets(selection, req);
		return true;
	}
	if (req->target == xwm->atoms[TIMESTAMP]) {
		send_timestamp(selection, req);
		return true;
	}
	if (req->target == xwm->atoms[DELETE]) {
		xwm_selection_send_notify(xwm, req, true);
		return true;
	}

	char *mime_type = xwm_mime_type_from_atom(xwm, req->target);
	if (mime_type == NULL) {
		nx_log(NX_ERROR, "ignoring selection request: unknown atom %u",
			req->target);
		return false;
	}
	bool sent = send_data(selection, req, mime_type);
	free(mime_type);
	return sent;
}

// Stale requests fail; others stay unanswered.
static void handle_foreign_request(struct nx_xwm_selection *selection,
		xcb_selection_request_event_t *req) {
	if (req->time != XCB_CURRENT_TIME && req->time < selection->timestamp) {
		nx_log(NX_DEBUG, "ignored old request from timestamp %d; expected > %d",
			req->time, selection->timestamp);
		xwm_selection_send_notify(selection->xwm, req, false);
		return;
	}
	nx_log(NX_DEBUG, "received selection request with invalid owner");
}

void xwm_handle_selection_request(struct nx_xwm *xwm,
		xcb_selection_request_event_t *req) {
	nx_log(NX_DEBUG, "XCB_SELECTION_REQUEST (time=%u owner=%u, requestor=%u "
		"selection=%u, target=%u, property=%u)",
		req->time, req->owner, req->requestor, req->selection, req->target,
		req->property);

	// Replied early, racing the copy.
	if (req->selection == xwm->atoms[CLIPBOARD_MANAGER]) {
		xwm_selection_send_notify(xwm, req, true);
		return;
	}

	struct nx_xwm_selection *selection = xwm_get_selection(xwm, req->selection);
	if (selection == NULL) {
		nx_log(NX_DEBUG, "received selection request for unknown selection");
		xwm_selection_send_notify(xwm, req, false);
		return;
	}
	if (req->requestor == selection->window) {
		nx_log(NX_ERROR, "selection request should have been caught before");
		xwm_selection_send_notify(xwm, req, false);
		return;
	}
	if (selection->window != req->owner) {
		handle_foreign_request(selection, req);
		return;
	}

	// Readable without X11 focus.
	if (!answer_request(selection, req)) {
		xwm_selection_send_notify(xwm, req, false);
	}
}

static void drop_requestor(struct nx_xwm_selection *selection,
		xcb_window_t requestor) {
	struct nx_xwm_selection_transfer *outgoing, *tmp;
	wl_list_for_each_safe(outgoing, tmp, &selection->outgoing, link) {
		if (outgoing->request.requestor == requestor) {
			xwm_selection_transfer_destroy_outgoing(outgoing);
		}
	}
}

void xwm_handle_selection_destroy_notify(struct nx_xwm *xwm,
		xcb_destroy_notify_event_t *event) {
	drop_requestor(&xwm->clipboard_selection, event->window);
	drop_requestor(&xwm->primary_selection, event->window);
	drop_requestor(&xwm->dnd_selection, event->window);
}
