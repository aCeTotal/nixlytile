#include "platform/xwayland/wm/xwm.h"

static bool is_own_window(struct nx_xwm *xwm, xcb_window_t window) {
	return window == xwm->window ||
		window == xwm->primary_selection.window ||
		window == xwm->clipboard_selection.window ||
		window == xwm->dnd_selection.window;
}

void xwm_handle_create_notify(struct nx_xwm *xwm, xcb_create_notify_event_t *ev) {
	if (is_own_window(xwm, ev->window)) {
		return;
	}
	xwm_surface_create(xwm, ev);
}

void xwm_handle_destroy_notify(struct nx_xwm *xwm,
		xcb_destroy_notify_event_t *ev) {
	struct nx_xwayland_surface *xsurface = xwm_lookup_surface(xwm, ev->window);
	if (xsurface == NULL) {
		return;
	}
	xwm_surface_destroy(xsurface);
	xwm_handle_selection_destroy_notify(xwm, ev);
}

void xwm_handle_configure_request(struct nx_xwm *xwm,
		xcb_configure_request_event_t *ev) {
	struct nx_xwayland_surface *xsurface = xwm_lookup_surface(xwm, ev->window);
	if (xsurface == NULL) {
		return;
	}

	uint16_t mask = ev->value_mask;
	uint16_t geo_mask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
		XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT;
	if ((mask & geo_mask) == 0) {
		return;
	}

	struct nx_xwayland_surface_configure_event event = {
		.surface = xsurface,
		.x = mask & XCB_CONFIG_WINDOW_X ? ev->x : xsurface->x,
		.y = mask & XCB_CONFIG_WINDOW_Y ? ev->y : xsurface->y,
		.width = mask & XCB_CONFIG_WINDOW_WIDTH ? ev->width : xsurface->width,
		.height = mask & XCB_CONFIG_WINDOW_HEIGHT ? ev->height : xsurface->height,
		.mask = mask,
	};
	wl_signal_emit_mutable(&xsurface->events.request_configure, &event);
}

void xwm_handle_configure_notify(struct nx_xwm *xwm,
		xcb_configure_notify_event_t *ev) {
	struct nx_xwayland_surface *xsurface = xwm_lookup_surface(xwm, ev->window);
	if (!xsurface) {
		return;
	}

	xsurface->x = ev->x;
	xsurface->y = ev->y;
	xsurface->width = ev->width;
	xsurface->height = ev->height;
	xwm_update_override_redirect(xsurface, ev->override_redirect);
}

void xwm_handle_map_request(struct nx_xwm *xwm, xcb_map_request_event_t *ev) {
	if (!xwm_lookup_surface(xwm, ev->window)) {
		return;
	}
	xcb_map_window(xwm->xcb_conn, ev->window);
}

void xwm_handle_map_notify(struct nx_xwm *xwm, xcb_map_notify_event_t *ev) {
	struct nx_xwayland_surface *xsurface = xwm_lookup_surface(xwm, ev->window);
	if (!xsurface) {
		return;
	}

	xwm_update_override_redirect(xsurface, ev->override_redirect);
	if (xsurface->override_redirect) {
		return;
	}
	xwm_surface_set_withdrawn(xsurface, false);
	nx_xwayland_surface_restack(xsurface, NULL, XCB_STACK_MODE_BELOW);
}

void xwm_handle_unmap_notify(struct nx_xwm *xwm, xcb_unmap_notify_event_t *ev) {
	struct nx_xwayland_surface *xsurface = xwm_lookup_surface(xwm, ev->window);
	if (xsurface == NULL) {
		return;
	}

	xwm_surface_dissociate(xsurface);
	if (!xsurface->override_redirect) {
		xwm_surface_set_withdrawn(xsurface, true);
	}
}
