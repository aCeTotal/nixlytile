#include <stdlib.h>
#include "platform/log/log.h"
#include "platform/xwayland/wm/xwm.h"

static bool parse_hints(struct nx_xwayland_surface *xsurface,
		xcb_get_property_reply_t *reply) {
	xsurface->hints = calloc(1, sizeof(*xsurface->hints));
	if (xsurface->hints == NULL) {
		return false;
	}
	xcb_icccm_get_wm_hints_from_reply(xsurface->hints, reply);
	// Missing input flag means input.
	if (!(xsurface->hints->flags & XCB_ICCCM_WM_HINT_INPUT)) {
		xsurface->hints->input = true;
	}
	return true;
}

void xwm_read_hints(struct nx_xwayland_surface *xsurface,
		xcb_get_property_reply_t *reply) {
	struct nx_xwm *xwm = xsurface->xwm;
	// ICCCM says WM_HINTS, clients send ATOM.
	if (reply->type != xwm->atoms[WM_HINTS] && reply->type != XCB_ATOM_ATOM &&
			reply->type != XCB_ATOM_NONE) {
		nx_log(NX_DEBUG, "Invalid WM_HINTS property type");
		return;
	}

	free(xsurface->hints);
	xsurface->hints = NULL;
	if (xcb_get_property_value_length(reply) > 0 && !parse_hints(xsurface, reply)) {
		return;
	}

	wl_signal_emit_mutable(&xsurface->events.set_hints, NULL);
}

// ICCCM min/base size fallback.
static void default_size_hints(xcb_size_hints_t *hints) {
	bool has_min = (hints->flags & XCB_ICCCM_SIZE_HINT_P_MIN_SIZE) != 0;
	bool has_base = (hints->flags & XCB_ICCCM_SIZE_HINT_BASE_SIZE) != 0;
	if (!has_min && !has_base) {
		hints->min_width = -1;
		hints->min_height = -1;
		hints->base_width = -1;
		hints->base_height = -1;
	} else if (!has_base) {
		hints->base_width = hints->min_width;
		hints->base_height = hints->min_height;
	} else if (!has_min) {
		hints->min_width = hints->base_width;
		hints->min_height = hints->base_height;
	}

	if ((hints->flags & XCB_ICCCM_SIZE_HINT_P_MAX_SIZE) == 0) {
		hints->max_width = -1;
		hints->max_height = -1;
	}
}

void xwm_read_normal_hints(struct nx_xwayland_surface *xsurface,
		xcb_get_property_reply_t *reply) {
	if (reply->type != xsurface->xwm->atoms[WM_SIZE_HINTS] &&
			reply->type != XCB_ATOM_NONE) {
		nx_log(NX_DEBUG, "Invalid WM_NORMAL_HINTS property type");
		return;
	}

	free(xsurface->size_hints);
	xsurface->size_hints = NULL;
	if (xcb_get_property_value_length(reply) == 0) {
		return;
	}

	xsurface->size_hints = calloc(1, sizeof(*xsurface->size_hints));
	if (xsurface->size_hints == NULL) {
		return;
	}
	xcb_icccm_get_wm_size_hints_from_reply(xsurface->size_hints, reply);
	default_size_hints(xsurface->size_hints);
}

enum nx_xwayland_icccm_input_model nx_xwayland_surface_icccm_input_model(
		const struct nx_xwayland_surface *xsurface) {
	bool take_focus = xwm_atoms_contains(xsurface->xwm, xsurface->protocols,
		xsurface->protocols_len, WM_TAKE_FOCUS);
	if (!xsurface->hints || xsurface->hints->input) {
		return take_focus ? NX_ICCCM_INPUT_MODEL_LOCAL : NX_ICCCM_INPUT_MODEL_PASSIVE;
	}
	return take_focus ? NX_ICCCM_INPUT_MODEL_GLOBAL : NX_ICCCM_INPUT_MODEL_NONE;
}
