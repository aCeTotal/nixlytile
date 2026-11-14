#include "platform/xwayland/wm/xwm.h"

#define PING_TIMEOUT_MS 10000

void xwm_send_wm_message(struct nx_xwayland_surface *xsurface,
		const xcb_client_message_data_t *data, uint32_t event_mask) {
	struct nx_xwm *xwm = xsurface->xwm;
	xcb_client_message_event_t event = {
		.response_type = XCB_CLIENT_MESSAGE,
		.format = 32,
		.window = xsurface->window_id,
		.type = xwm->atoms[WM_PROTOCOLS],
		.data = *data,
	};
	xwm_send_event(xwm, xsurface->window_id, event_mask, &event, sizeof(event));
	xwm_schedule_flush(xwm);
}

// ICCCM 4.1.5: size unchanged.
static void send_synthetic_configure(struct nx_xwayland_surface *xsurface) {
	xcb_configure_notify_event_t event = {
		.response_type = XCB_CONFIGURE_NOTIFY,
		.event = xsurface->window_id,
		.window = xsurface->window_id,
		.x = xsurface->x,
		.y = xsurface->y,
		.width = xsurface->width,
		.height = xsurface->height,
	};
	xwm_send_event(xsurface->xwm, xsurface->window_id,
		XCB_EVENT_MASK_STRUCTURE_NOTIFY, &event, sizeof(event));
}

void nx_xwayland_surface_configure(struct nx_xwayland_surface *xsurface,
		int16_t x, int16_t y, uint16_t width, uint16_t height) {
	struct nx_xwm *xwm = xsurface->xwm;
	bool same_size = width == xsurface->width && height == xsurface->height;

	xsurface->x = x;
	xsurface->y = y;
	xsurface->width = width;
	xsurface->height = height;

	uint32_t mask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
		XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT |
		XCB_CONFIG_WINDOW_BORDER_WIDTH;
	uint32_t values[] = { x, y, width, height, 0 };
	xcb_configure_window(xwm->xcb_conn, xsurface->window_id, mask, values);

	// ICCCM skips override-redirect windows.
	if (same_size && !xsurface->override_redirect) {
		send_synthetic_configure(xsurface);
	}
	xwm_schedule_flush(xwm);
}

void nx_xwayland_surface_close(struct nx_xwayland_surface *xsurface) {
	struct nx_xwm *xwm = xsurface->xwm;

	if (!xwm_atoms_contains(xwm, xsurface->protocols, xsurface->protocols_len,
			WM_DELETE_WINDOW)) {
		xcb_kill_client(xwm->xcb_conn, xsurface->window_id);
		xwm_schedule_flush(xwm);
		return;
	}

	xcb_client_message_data_t message = {
		.data32 = { xwm->atoms[WM_DELETE_WINDOW], XCB_CURRENT_TIME },
	};
	xwm_send_wm_message(xsurface, &message, XCB_EVENT_MASK_NO_EVENT);
}

void nx_xwayland_surface_ping(struct nx_xwayland_surface *xsurface) {
	xcb_client_message_data_t message = {
		.data32 = {
			xsurface->xwm->atoms[NET_WM_PING],
			XCB_CURRENT_TIME,
			xsurface->window_id,
		},
	};
	xwm_send_wm_message(xsurface, &message, XCB_EVENT_MASK_NO_EVENT);

	wl_event_source_timer_update(xsurface->ping_timer, PING_TIMEOUT_MS);
	xsurface->pinging = true;
}
