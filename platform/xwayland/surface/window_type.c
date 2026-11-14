#include "platform/xwayland/wm/xwm.h"

bool nx_xwayland_surface_has_window_type(
		const struct nx_xwayland_surface *xsurface,
		enum nx_xwayland_net_wm_window_type window_type) {
	static const enum atom_name atom_names[] = {
		[NX_XWAYLAND_NET_WM_WINDOW_TYPE_TOOLBAR] = NET_WM_WINDOW_TYPE_TOOLBAR,
		[NX_XWAYLAND_NET_WM_WINDOW_TYPE_MENU] = NET_WM_WINDOW_TYPE_MENU,
		[NX_XWAYLAND_NET_WM_WINDOW_TYPE_UTILITY] = NET_WM_WINDOW_TYPE_UTILITY,
		[NX_XWAYLAND_NET_WM_WINDOW_TYPE_SPLASH] = NET_WM_WINDOW_TYPE_SPLASH,
		[NX_XWAYLAND_NET_WM_WINDOW_TYPE_DIALOG] = NET_WM_WINDOW_TYPE_DIALOG,
		[NX_XWAYLAND_NET_WM_WINDOW_TYPE_DROPDOWN_MENU] = NET_WM_WINDOW_TYPE_DROPDOWN_MENU,
		[NX_XWAYLAND_NET_WM_WINDOW_TYPE_POPUP_MENU] = NET_WM_WINDOW_TYPE_POPUP_MENU,
		[NX_XWAYLAND_NET_WM_WINDOW_TYPE_TOOLTIP] = NET_WM_WINDOW_TYPE_TOOLTIP,
		[NX_XWAYLAND_NET_WM_WINDOW_TYPE_NOTIFICATION] = NET_WM_WINDOW_TYPE_NOTIFICATION,
		[NX_XWAYLAND_NET_WM_WINDOW_TYPE_COMBO] = NET_WM_WINDOW_TYPE_COMBO,
		[NX_XWAYLAND_NET_WM_WINDOW_TYPE_DND] = NET_WM_WINDOW_TYPE_DND,
	};
	return xwm_atoms_contains(xsurface->xwm, xsurface->window_type,
		xsurface->window_type_len, atom_names[window_type]);
}

bool nx_xwayland_surface_override_redirect_wants_focus(
		const struct nx_xwayland_surface *xsurface) {
	static const enum atom_name needles[] = {
		NET_WM_WINDOW_TYPE_COMBO,
		NET_WM_WINDOW_TYPE_DND,
		NET_WM_WINDOW_TYPE_DROPDOWN_MENU,
		NET_WM_WINDOW_TYPE_MENU,
		NET_WM_WINDOW_TYPE_NOTIFICATION,
		NET_WM_WINDOW_TYPE_POPUP_MENU,
		NET_WM_WINDOW_TYPE_SPLASH,
		NET_WM_WINDOW_TYPE_DESKTOP,
		NET_WM_WINDOW_TYPE_TOOLTIP,
		NET_WM_WINDOW_TYPE_UTILITY,
	};

	for (size_t i = 0; i < sizeof(needles) / sizeof(needles[0]); i++) {
		if (xwm_atoms_contains(xsurface->xwm, xsurface->window_type,
				xsurface->window_type_len, needles[i])) {
			return false;
		}
	}
	return true;
}
