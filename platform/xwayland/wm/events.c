#include <assert.h>
#include <inttypes.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include "platform/log/log.h"
#include "platform/xwayland/wm/xwm.h"

#define X11_EVENT_SIZE 32

void xwm_send_event(struct nx_xwm *xwm, xcb_window_t destination,
		uint32_t event_mask, const void *event, size_t length) {
	char buf[X11_EVENT_SIZE] = { 0 };
	assert(length <= sizeof(buf));
	memcpy(buf, event, length);
	xcb_send_event(xwm->xcb_conn, 0, destination, event_mask, buf);
}

void xwm_schedule_flush(struct nx_xwm *xwm) {
	struct pollfd pollfd = {
		.fd = xcb_get_file_descriptor(xwm->xcb_conn),
		.events = POLLOUT,
	};
	if (poll(&pollfd, 1, 0) < 0) {
		nx_log(NX_ERROR, "poll() failed");
		return;
	}

	if (pollfd.revents & POLLOUT) {
		xcb_flush(xwm->xcb_conn);
		return;
	}

	wl_event_source_fd_update(xwm->event_source,
		WL_EVENT_READABLE | WL_EVENT_WRITABLE);
}

static void log_raw_error(const xcb_value_error_t *ev) {
	nx_log(NX_ERROR, "xcb error: op %" PRIu8 ":%" PRIu16 ", code %" PRIu8
		", sequence %" PRIu16 ", value %" PRIu32,
		ev->major_opcode, ev->minor_opcode, ev->error_code,
		ev->sequence, ev->bad_value);
}

static void log_error(struct nx_xwm *xwm, const xcb_value_error_t *ev) {
	const char *major_name = xcb_errors_get_name_for_major_code(
		xwm->errors_context, ev->major_opcode);
	if (!major_name) {
		nx_log(NX_DEBUG, "xcb error happened, but could not get major name");
		log_raw_error(ev);
		return;
	}

	const char *minor_name = xcb_errors_get_name_for_minor_code(
		xwm->errors_context, ev->major_opcode, ev->minor_opcode);
	const char *extension;
	const char *error_name = xcb_errors_get_name_for_error(
		xwm->errors_context, ev->error_code, &extension);
	if (!error_name) {
		nx_log(NX_DEBUG, "xcb error happened, but could not get error name");
		log_raw_error(ev);
		return;
	}

	nx_log(NX_ERROR, "xcb error: op %s (%s), code %s (%s), sequence %" PRIu16
		", value %" PRIu32,
		major_name, minor_name ? minor_name : "no minor",
		error_name, extension ? extension : "no extension",
		ev->sequence, ev->bad_value);
}

static void log_unhandled_event(struct nx_xwm *xwm, xcb_generic_event_t *ev) {
	const char *extension;
	const char *event_name = xcb_errors_get_name_for_xcb_event(
		xwm->errors_context, ev, &extension);
	if (!event_name) {
		nx_log(NX_DEBUG, "no name for unhandled event: %u", ev->response_type);
		return;
	}
	nx_log(NX_DEBUG, "unhandled X11 event: %s (%u)", event_name,
		ev->response_type);
}

static void handle_event(struct nx_xwm *xwm, xcb_generic_event_t *event) {
	switch (event->response_type & XCB_EVENT_RESPONSE_TYPE_MASK) {
	case XCB_CREATE_NOTIFY:
		xwm_handle_create_notify(xwm, (xcb_create_notify_event_t *)event);
		break;
	case XCB_DESTROY_NOTIFY:
		xwm_handle_destroy_notify(xwm, (xcb_destroy_notify_event_t *)event);
		break;
	case XCB_CONFIGURE_REQUEST:
		xwm_handle_configure_request(xwm, (xcb_configure_request_event_t *)event);
		break;
	case XCB_CONFIGURE_NOTIFY:
		xwm_handle_configure_notify(xwm, (xcb_configure_notify_event_t *)event);
		break;
	case XCB_MAP_REQUEST:
		xwm_handle_map_request(xwm, (xcb_map_request_event_t *)event);
		break;
	case XCB_MAP_NOTIFY:
		xwm_handle_map_notify(xwm, (xcb_map_notify_event_t *)event);
		break;
	case XCB_UNMAP_NOTIFY:
		xwm_handle_unmap_notify(xwm, (xcb_unmap_notify_event_t *)event);
		break;
	case XCB_PROPERTY_NOTIFY:
		xwm_handle_property_notify(xwm, (xcb_property_notify_event_t *)event);
		break;
	case XCB_CLIENT_MESSAGE:
		xwm_handle_client_message(xwm, (xcb_client_message_event_t *)event);
		break;
	case XCB_FOCUS_IN:
		xwm_handle_focus_in(xwm, (xcb_focus_in_event_t *)event);
		break;
	case 0:
		log_error(xwm, (xcb_value_error_t *)event);
		break;
	default:
		log_unhandled_event(xwm, event);
		break;
	}
}

static int read_x11_events(struct nx_xwm *xwm) {
	int count = 0;
	xcb_generic_event_t *event;
	while ((event = xcb_poll_for_event(xwm->xcb_conn))) {
		count++;
		if (!xwm_handle_selection_event(xwm, event)) {
			handle_event(xwm, event);
		}
		free(event);
	}
	return count;
}

int xwm_handle_x11_events(int fd, uint32_t mask, void *data) {
	struct nx_xwm *xwm = data;

	if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
		xwm_destroy(xwm);
		return 0;
	}

	int count = 0;
	if (mask & WL_EVENT_READABLE) {
		count = read_x11_events(xwm);
	}
	if (count) {
		xwm_schedule_flush(xwm);
	}

	if (mask & WL_EVENT_WRITABLE) {
		// xcb_flush blocks until written.
		xcb_flush(xwm->xcb_conn);
		wl_event_source_fd_update(xwm->event_source, WL_EVENT_READABLE);
	}

	return count;
}
