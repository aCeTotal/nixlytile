#include <assert.h>
#include <stdlib.h>
#include "platform/surface/compositor.h"
#include "platform/xwayland/wm/xwm.h"

static bool is_mapped(const struct nx_xwayland_surface *xsurface) {
	return xsurface->surface != NULL && xsurface->surface->mapped;
}

// Creation order, not map order.
void xwm_update_client_list(struct nx_xwm *xwm) {
	size_t count = 0;
	struct nx_xwayland_surface *xsurface;
	wl_list_for_each(xsurface, &xwm->surfaces, link) {
		count += is_mapped(xsurface);
	}

	xcb_window_t *windows = count > 0 ? malloc(count * sizeof(*windows)) : NULL;
	if (count > 0 && !windows) {
		return;
	}

	size_t i = 0;
	wl_list_for_each(xsurface, &xwm->surfaces, link) {
		if (is_mapped(xsurface)) {
			windows[i++] = xsurface->window_id;
		}
	}

	xcb_change_property(xwm->xcb_conn, XCB_PROP_MODE_REPLACE,
		xwm->screen->root, xwm->atoms[NET_CLIENT_LIST],
		XCB_ATOM_WINDOW, 32, count, windows);
	free(windows);
}

void xwm_update_client_list_stacking(struct nx_xwm *xwm) {
	size_t count = wl_list_length(&xwm->surfaces_in_stack_order);
	xcb_window_t *windows = malloc(sizeof(*windows) * count);
	if (!windows) {
		return;
	}

	size_t i = 0;
	struct nx_xwayland_surface *xsurface;
	wl_list_for_each(xsurface, &xwm->surfaces_in_stack_order, stack_link) {
		windows[i++] = xsurface->window_id;
	}

	xcb_change_property(xwm->xcb_conn, XCB_PROP_MODE_REPLACE,
		xwm->screen->root, xwm->atoms[NET_CLIENT_LIST_STACKING],
		XCB_ATOM_WINDOW, 32, count, windows);
	free(windows);
}

// The stack_link to insert after.
static struct wl_list *stack_position(struct nx_xwm *xwm,
		struct nx_xwayland_surface *sibling, enum xcb_stack_mode_t mode) {
	switch (mode) {
	case XCB_STACK_MODE_ABOVE:
		return &sibling->stack_link;
	case XCB_STACK_MODE_BELOW:
		return sibling ? sibling->stack_link.prev : &xwm->surfaces_in_stack_order;
	default:
		abort(); // other modes are unsupported
	}
}

void nx_xwayland_surface_restack(struct nx_xwayland_surface *xsurface,
		struct nx_xwayland_surface *sibling, enum xcb_stack_mode_t mode) {
	struct nx_xwm *xwm = xsurface->xwm;
	assert(!xsurface->override_redirect);

	// Stay below override-redirect windows.
	if (mode == XCB_STACK_MODE_ABOVE && !sibling) {
		sibling = wl_container_of(xwm->surfaces_in_stack_order.prev,
			sibling, stack_link);
	}
	if (sibling == xsurface) {
		return;
	}

	uint32_t values[2];
	size_t idx = 0;
	uint32_t flags = XCB_CONFIG_WINDOW_STACK_MODE;
	if (sibling != NULL) {
		values[idx++] = sibling->window_id;
		flags |= XCB_CONFIG_WINDOW_SIBLING;
	}
	values[idx++] = mode;
	xcb_configure_window(xwm->xcb_conn, xsurface->window_id, flags, values);

	wl_list_remove(&xsurface->stack_link);
	wl_list_insert(stack_position(xwm, sibling, mode), &xsurface->stack_link);
	xwm_update_client_list_stacking(xwm);
	xwm_schedule_flush(xwm);
}

void xwm_update_override_redirect(struct nx_xwayland_surface *xsurface,
		bool override_redirect) {
	if (xsurface->override_redirect == override_redirect) {
		return;
	}
	xsurface->override_redirect = override_redirect;

	if (override_redirect) {
		wl_list_remove(&xsurface->stack_link);
		wl_list_init(&xsurface->stack_link);
		xwm_update_client_list_stacking(xsurface->xwm);
	} else if (is_mapped(xsurface)) {
		nx_xwayland_surface_restack(xsurface, NULL, XCB_STACK_MODE_BELOW);
	}

	wl_signal_emit_mutable(&xsurface->events.set_override_redirect, NULL);
}
