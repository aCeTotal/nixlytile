#include <inttypes.h>
#include <stddef.h>
#include "platform/log/log.h"
#include "platform/xwayland/wm/xwm.h"

enum net_wm_state_action {
	NET_WM_STATE_REMOVE = 0,
	NET_WM_STATE_ADD = 1,
	NET_WM_STATE_TOGGLE = 2,
};

// In _NET_WM_STATE property order.
static const struct {
	enum atom_name atom;
	size_t offset;
} net_wm_states[] = {
	{ NET_WM_STATE_MODAL, offsetof(struct nx_xwayland_surface, modal) },
	{ NET_WM_STATE_FULLSCREEN, offsetof(struct nx_xwayland_surface, fullscreen) },
	{ NET_WM_STATE_MAXIMIZED_VERT, offsetof(struct nx_xwayland_surface, maximized_vert) },
	{ NET_WM_STATE_MAXIMIZED_HORZ, offsetof(struct nx_xwayland_surface, maximized_horz) },
	{ NET_WM_STATE_HIDDEN, offsetof(struct nx_xwayland_surface, minimized) },
	{ NET_WM_STATE_STICKY, offsetof(struct nx_xwayland_surface, sticky) },
	{ NET_WM_STATE_SHADED, offsetof(struct nx_xwayland_surface, shaded) },
	{ NET_WM_STATE_SKIP_TASKBAR, offsetof(struct nx_xwayland_surface, skip_taskbar) },
	{ NET_WM_STATE_SKIP_PAGER, offsetof(struct nx_xwayland_surface, skip_pager) },
	{ NET_WM_STATE_ABOVE, offsetof(struct nx_xwayland_surface, above) },
	{ NET_WM_STATE_BELOW, offsetof(struct nx_xwayland_surface, below) },
	{ NET_WM_STATE_DEMANDS_ATTENTION, offsetof(struct nx_xwayland_surface, demands_attention) },
};

#define NET_WM_STATE_COUNT (sizeof(net_wm_states) / sizeof(net_wm_states[0]))

static bool *state_flag(struct nx_xwayland_surface *xsurface, size_t index) {
	return (bool *)((char *)xsurface + net_wm_states[index].offset);
}

static bool *find_state(struct nx_xwayland_surface *xsurface, xcb_atom_t atom) {
	for (size_t i = 0; i < NET_WM_STATE_COUNT; i++) {
		if (xsurface->xwm->atoms[net_wm_states[i].atom] == atom) {
			return state_flag(xsurface, i);
		}
	}
	return NULL;
}

// Only fullscreen resets; others accumulate.
void xwm_read_net_wm_state(struct nx_xwayland_surface *xsurface,
		xcb_get_property_reply_t *reply) {
	const xcb_atom_t *atoms = xcb_get_property_value(reply);
	size_t count = xcb_get_property_value_length(reply) / sizeof(atoms[0]);

	xsurface->fullscreen = false;
	for (size_t i = 0; i < count; i++) {
		bool *flag = find_state(xsurface, atoms[i]);
		if (flag != NULL) {
			*flag = true;
		}
	}
}

void xwm_surface_update_net_wm_state(struct nx_xwayland_surface *xsurface) {
	struct nx_xwm *xwm = xsurface->xwm;

	// EWMH: withdrawn windows have no state.
	if (xsurface->withdrawn) {
		xcb_delete_property(xwm->xcb_conn, xsurface->window_id,
			xwm->atoms[NET_WM_STATE]);
		return;
	}

	uint32_t property[NET_WM_STATE_COUNT + 1];
	size_t len = 0;
	for (size_t i = 0; i < NET_WM_STATE_COUNT; i++) {
		if (*state_flag(xsurface, i)) {
			property[len++] = xwm->atoms[net_wm_states[i].atom];
		}
	}
	if (xsurface == xwm->focus_surface) {
		property[len++] = xwm->atoms[NET_WM_STATE_FOCUSED];
	}

	xcb_change_property(xwm->xcb_conn, XCB_PROP_MODE_REPLACE,
		xsurface->window_id, xwm->atoms[NET_WM_STATE], XCB_ATOM_ATOM, 32,
		len, property);
}

static void update_wm_state(struct nx_xwayland_surface *xsurface) {
	struct nx_xwm *xwm = xsurface->xwm;
	uint32_t property[] = { XCB_ICCCM_WM_STATE_NORMAL, XCB_WINDOW_NONE };
	if (xsurface->withdrawn) {
		property[0] = XCB_ICCCM_WM_STATE_WITHDRAWN;
	} else if (xsurface->minimized) {
		property[0] = XCB_ICCCM_WM_STATE_ICONIC;
	}

	xcb_change_property(xwm->xcb_conn, XCB_PROP_MODE_REPLACE,
		xsurface->window_id, xwm->atoms[WM_STATE], xwm->atoms[WM_STATE], 32,
		sizeof(property) / sizeof(property[0]), property);
}

// Returns whether the state changed.
static bool update_state(uint32_t action, bool *state) {
	bool new_state;
	switch (action) {
	case NET_WM_STATE_REMOVE:
		new_state = false;
		break;
	case NET_WM_STATE_ADD:
		new_state = true;
		break;
	case NET_WM_STATE_TOGGLE:
		new_state = !*state;
		break;
	default:
		return false;
	}

	bool changed = *state != new_state;
	*state = new_state;
	return changed;
}

static void apply_state_change(struct nx_xwayland_surface *xsurface,
		uint32_t action, xcb_atom_t atom) {
	bool *flag = find_state(xsurface, atom);
	if (flag == NULL && atom != XCB_ATOM_NONE) {
		nx_log(NX_DEBUG, "Unhandled NET_WM_STATE property change %" PRIu32, atom);
	}
	if (flag != NULL && update_state(action, flag)) {
		xwm_surface_update_net_wm_state(xsurface);
	}
}

void xwm_handle_net_wm_state_message(struct nx_xwm *xwm,
		xcb_client_message_event_t *ev) {
	struct nx_xwayland_surface *xsurface = xwm_lookup_surface(xwm, ev->window);
	if (!xsurface || ev->format != 32) {
		return;
	}

	bool fullscreen = xsurface->fullscreen;
	bool minimized = xsurface->minimized;

	// Two states per message.
	uint32_t action = ev->data.data32[0];
	apply_state_change(xsurface, action, ev->data.data32[1]);
	apply_state_change(xsurface, action, ev->data.data32[2]);

	if (fullscreen != xsurface->fullscreen) {
		wl_signal_emit_mutable(&xsurface->events.request_fullscreen, NULL);
	}
	if (minimized != xsurface->minimized) {
		struct nx_xwayland_minimize_event event = {
			.surface = xsurface,
			.minimize = xsurface->minimized,
		};
		wl_signal_emit_mutable(&xsurface->events.request_minimize, &event);
	}
}

void xwm_surface_set_withdrawn(struct nx_xwayland_surface *xsurface,
		bool withdrawn) {
	xsurface->withdrawn = withdrawn;
	update_wm_state(xsurface);
	xwm_surface_update_net_wm_state(xsurface);
	xwm_schedule_flush(xsurface->xwm);
}

void nx_xwayland_surface_set_minimized(struct nx_xwayland_surface *xsurface,
		bool minimized) {
	xsurface->minimized = minimized;
	update_wm_state(xsurface);
	xwm_surface_update_net_wm_state(xsurface);
	xwm_schedule_flush(xsurface->xwm);
}

void nx_xwayland_surface_set_maximized(struct nx_xwayland_surface *xsurface,
		bool maximized_horz, bool maximized_vert) {
	xsurface->maximized_horz = maximized_horz;
	xsurface->maximized_vert = maximized_vert;
	xwm_surface_update_net_wm_state(xsurface);
	xwm_schedule_flush(xsurface->xwm);
}

void nx_xwayland_surface_set_fullscreen(struct nx_xwayland_surface *xsurface,
		bool fullscreen) {
	xsurface->fullscreen = fullscreen;
	xwm_surface_update_net_wm_state(xsurface);
	xwm_schedule_flush(xsurface->xwm);
}
