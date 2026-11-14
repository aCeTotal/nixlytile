#include <inttypes.h>
#include "platform/log/log.h"
#include "platform/xwayland/wm/xwm.h"

typedef void (*message_fn)(struct nx_xwm *xwm,
	xcb_client_message_event_t *ev);

// The client answered our _NET_WM_PING.
static void handle_wm_protocols_message(struct nx_xwm *xwm,
		xcb_client_message_event_t *ev) {
	xcb_atom_t type = ev->data.data32[0];
	if (type != xwm->atoms[NET_WM_PING]) {
		nx_log(NX_DEBUG, "unhandled WM_PROTOCOLS client message %" PRIu32, type);
		return;
	}

	struct nx_xwayland_surface *xsurface =
		xwm_lookup_surface(xwm, ev->data.data32[2]);
	if (xsurface == NULL || !xsurface->pinging) {
		return;
	}

	wl_event_source_timer_update(xsurface->ping_timer, 0);
	xsurface->pinging = false;
}

static void handle_net_active_window_message(struct nx_xwm *xwm,
		xcb_client_message_event_t *ev) {
	struct nx_xwayland_surface *xsurface = xwm_lookup_surface(xwm, ev->window);
	if (xsurface == NULL) {
		return;
	}
	wl_signal_emit_mutable(&xsurface->events.request_activate, NULL);
}

static void handle_wm_change_state_message(struct nx_xwm *xwm,
		xcb_client_message_event_t *ev) {
	struct nx_xwayland_surface *xsurface = xwm_lookup_surface(xwm, ev->window);
	if (xsurface == NULL) {
		return;
	}

	uint32_t detail = ev->data.data32[0];
	bool minimize;
	switch (detail) {
	case XCB_ICCCM_WM_STATE_ICONIC:
		minimize = true;
		break;
	case XCB_ICCCM_WM_STATE_NORMAL:
		minimize = false;
		break;
	default:
		nx_log(NX_DEBUG, "unhandled wm_change_state event %u", detail);
		return;
	}

	struct nx_xwayland_minimize_event event = {
		.surface = xsurface,
		.minimize = minimize,
	};
	wl_signal_emit_mutable(&xsurface->events.request_minimize, &event);
}

static const struct {
	enum atom_name type;
	message_fn dispatch;
} client_messages[] = {
	{ WL_SURFACE_ID, xwm_handle_surface_id_message },
	{ WL_SURFACE_SERIAL, xwm_handle_surface_serial_message },
	{ NET_WM_STATE, xwm_handle_net_wm_state_message },
	{ WM_PROTOCOLS, handle_wm_protocols_message },
	{ NET_ACTIVE_WINDOW, handle_net_active_window_message },
	{ WM_CHANGE_STATE, handle_wm_change_state_message },
};

void xwm_handle_client_message(struct nx_xwm *xwm,
		xcb_client_message_event_t *ev) {
	size_t count = sizeof(client_messages) / sizeof(client_messages[0]);
	for (size_t i = 0; i < count; i++) {
		if (ev->type == xwm->atoms[client_messages[i].type]) {
			client_messages[i].dispatch(xwm, ev);
			return;
		}
	}

	if (!xwm_handle_selection_client_message(xwm, ev)) {
		nx_log(NX_DEBUG, "unhandled x11 client message %" PRIu32, ev->type);
	}
}
