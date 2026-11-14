#include "platform/xwayland/wm/xwm.h"

// Missing from the xcb headers.
static const xcb_window_t pointer_root = 1;

void xwm_set_net_active_window(struct nx_xwm *xwm, xcb_window_t window) {
	xcb_change_property(xwm->xcb_conn, XCB_PROP_MODE_REPLACE,
		xwm->screen->root, xwm->atoms[NET_ACTIVE_WINDOW],
		xwm->atoms[WINDOW], 32, 1, &window);
}

// Gives keyboard focus; also denies FocusIn.
static void focus_window(struct nx_xwm *xwm,
		struct nx_xwayland_surface *xsurface) {
	if (!xsurface) {
		// XCB_NONE breaks popup keyboard grabs.
		xcb_set_input_focus_checked(xwm->xcb_conn,
			XCB_INPUT_FOCUS_POINTER_ROOT, pointer_root, XCB_CURRENT_TIME);
		return;
	}
	if (xsurface->override_redirect) {
		return;
	}

	xcb_client_message_data_t message = {
		.data32 = { xwm->atoms[WM_TAKE_FOCUS], XCB_TIME_CURRENT_TIME },
	};

	// Input-less clients focus themselves.
	if (xsurface->hints && !xsurface->hints->input) {
		xwm_send_wm_message(xsurface, &message, XCB_EVENT_MASK_NO_EVENT);
		return;
	}

	xwm_send_wm_message(xsurface, &message, XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT);
	xcb_void_cookie_t cookie = xcb_set_input_focus(xwm->xcb_conn,
		XCB_INPUT_FOCUS_POINTER_ROOT, xsurface->window_id, XCB_CURRENT_TIME);
	xwm->last_focus_seq = cookie.sequence;
}

// Updates _NET_ACTIVE_WINDOW and _NET_WM_STATE.
static void set_focused_window(struct nx_xwm *xwm,
		struct nx_xwayland_surface *xsurface) {
	struct nx_xwayland_surface *unfocused = xwm->focus_surface;
	if (xsurface && xsurface->override_redirect) {
		return;
	}

	xwm->focus_surface = xsurface;
	if (xsurface == unfocused) {
		return;
	}

	if (unfocused) {
		xwm_surface_update_net_wm_state(unfocused);
	}
	if (!xsurface) {
		xwm_set_net_active_window(xwm, xwm->no_focus_window);
		return;
	}
	xwm_surface_update_net_wm_state(xsurface);
	xwm_set_net_active_window(xwm, xsurface->window_id);
}

void xwm_surface_activate(struct nx_xwm *xwm,
		struct nx_xwayland_surface *xsurface) {
	if (xsurface && xsurface->override_redirect) {
		return;
	}

	if (xsurface != xwm->focus_surface) {
		focus_window(xwm, xsurface);
	}
	set_focused_window(xwm, xsurface);
	xwm_schedule_flush(xwm);
}

void nx_xwayland_surface_activate(struct nx_xwayland_surface *xsurface,
		bool activated) {
	struct nx_xwm *xwm = xsurface->xwm;
	if (activated) {
		xwm_surface_activate(xwm, xsurface);
		return;
	}
	if (xwm->focus_surface == xsurface) {
		xwm_surface_activate(xwm, NULL);
	}
}

// Rejects FocusIn older than ours.
static bool validate_focus_serial(uint16_t last_focus_seq, uint16_t event_seq) {
	uint16_t rev_dist = event_seq - last_focus_seq;
	return rev_dist < UINT16_MAX / 2;
}

void xwm_handle_focus_in(struct nx_xwm *xwm, xcb_focus_in_event_t *ev) {
	if (ev->detail == XCB_NOTIFY_DETAIL_POINTER) {
		return;
	}
	// Leave keyboard grabs alone.
	if (ev->mode == XCB_NOTIFY_MODE_GRAB || ev->mode == XCB_NOTIFY_MODE_UNGRAB) {
		return;
	}
	if (!validate_focus_serial(xwm->last_focus_seq, ev->sequence)) {
		return;
	}

	// Same-app focus moves allowed (Steam).
	struct nx_xwayland_surface *xsurface = xwm_lookup_surface(xwm, ev->event);
	struct nx_xwayland_surface *focused = xwm->focus_surface;
	if (xsurface && focused && xsurface->pid == focused->pid) {
		set_focused_window(xwm, xsurface);
		return;
	}

	// Refocus the window that had focus.
	focus_window(xwm, focused);
}
