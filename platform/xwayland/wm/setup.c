#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/composite.h>
#include <xcb/res.h>
#include <xcb/xfixes.h>
#include "platform/log/log.h"
#include "platform/xwayland/wm/xwm.h"

static const char wm_name[] = "nixlytile wm";

static void query_xfixes(struct nx_xwm *xwm) {
	xwm->xfixes = xcb_get_extension_data(xwm->xcb_conn, &xcb_xfixes_id);
	if (!xwm->xfixes || !xwm->xfixes->present) {
		nx_log(NX_DEBUG, "xfixes not available");
	}

	xcb_xfixes_query_version_cookie_t cookie = xcb_xfixes_query_version(
		xwm->xcb_conn, XCB_XFIXES_MAJOR_VERSION, XCB_XFIXES_MINOR_VERSION);
	xcb_xfixes_query_version_reply_t *reply =
		xcb_xfixes_query_version_reply(xwm->xcb_conn, cookie, NULL);

	nx_log(NX_DEBUG, "xfixes version: %" PRIu32 ".%" PRIu32,
		reply->major_version, reply->minor_version);
	xwm->xfixes_major_version = reply->major_version;
	free(reply);
}

// Client pids need XRes 1.2.
static void query_xres(struct nx_xwm *xwm) {
	const xcb_query_extension_reply_t *xres =
		xcb_get_extension_data(xwm->xcb_conn, &xcb_res_id);
	if (!xres || !xres->present) {
		return;
	}

	xcb_res_query_version_cookie_t cookie = xcb_res_query_version(
		xwm->xcb_conn, XCB_RES_MAJOR_VERSION, XCB_RES_MINOR_VERSION);
	xcb_res_query_version_reply_t *reply =
		xcb_res_query_version_reply(xwm->xcb_conn, cookie, NULL);
	if (reply == NULL) {
		return;
	}

	nx_log(NX_DEBUG, "xres version: %" PRIu32 ".%" PRIu32,
		reply->server_major, reply->server_minor);
	if (reply->server_major > 1 ||
			(reply->server_major == 1 && reply->server_minor >= 2)) {
		xwm->xres = xres;
	}
	free(reply);
}

void xwm_query_resources(struct nx_xwm *xwm) {
	xcb_prefetch_extension_data(xwm->xcb_conn, &xcb_xfixes_id);
	xcb_prefetch_extension_data(xwm->xcb_conn, &xcb_composite_id);
	xcb_prefetch_extension_data(xwm->xcb_conn, &xcb_res_id);

	if (!xwm_intern_atoms(xwm)) {
		return;
	}
	query_xfixes(xwm);
	query_xres(xwm);
}

static void set_supported(struct nx_xwm *xwm) {
	xcb_atom_t supported[] = {
		xwm->atoms[NET_WM_STATE],
		xwm->atoms[NET_ACTIVE_WINDOW],
		xwm->atoms[NET_CLOSE_WINDOW],
		xwm->atoms[NET_WM_MOVERESIZE],
		xwm->atoms[NET_WM_STATE_FOCUSED],
		xwm->atoms[NET_WM_STATE_MODAL],
		xwm->atoms[NET_WM_STATE_FULLSCREEN],
		xwm->atoms[NET_WM_STATE_MAXIMIZED_VERT],
		xwm->atoms[NET_WM_STATE_MAXIMIZED_HORZ],
		xwm->atoms[NET_WM_STATE_HIDDEN],
		xwm->atoms[NET_WM_STATE_STICKY],
		xwm->atoms[NET_WM_STATE_SHADED],
		xwm->atoms[NET_WM_STATE_SKIP_TASKBAR],
		xwm->atoms[NET_WM_STATE_SKIP_PAGER],
		xwm->atoms[NET_WM_STATE_ABOVE],
		xwm->atoms[NET_WM_STATE_BELOW],
		xwm->atoms[NET_WM_STATE_DEMANDS_ATTENTION],
		xwm->atoms[NET_CLIENT_LIST],
		xwm->atoms[NET_CLIENT_LIST_STACKING],
	};
	xcb_change_property(xwm->xcb_conn, XCB_PROP_MODE_REPLACE,
		xwm->screen->root, xwm->atoms[NET_SUPPORTED], XCB_ATOM_ATOM, 32,
		sizeof(supported) / sizeof(supported[0]), supported);
}

void xwm_manage_root(struct nx_xwm *xwm) {
	uint32_t event_mask = XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY |
		XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT |
		XCB_EVENT_MASK_PROPERTY_CHANGE;
	xcb_change_window_attributes(xwm->xcb_conn, xwm->screen->root,
		XCB_CW_EVENT_MASK, &event_mask);

	xcb_composite_redirect_subwindows(xwm->xcb_conn, xwm->screen->root,
		XCB_COMPOSITE_REDIRECT_MANUAL);

	set_supported(xwm);

	// Exit with the last client.
	if (xwm->xfixes_major_version >= 6) {
		xcb_xfixes_set_client_disconnect_mode(xwm->xcb_conn,
			XCB_XFIXES_CLIENT_DISCONNECT_FLAGS_TERMINATE);
	}

	xcb_flush(xwm->xcb_conn);
}

// The _NET_SUPPORTING_WM_CHECK window owning WM_S0.
static void create_wm_window(struct nx_xwm *xwm) {
	xwm->window = xcb_generate_id(xwm->xcb_conn);
	xcb_create_window(xwm->xcb_conn, XCB_COPY_FROM_PARENT, xwm->window,
		xwm->screen->root, 0, 0, 10, 10, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
		xwm->screen->root_visual, 0, NULL);

	xcb_change_property(xwm->xcb_conn, XCB_PROP_MODE_REPLACE, xwm->window,
		xwm->atoms[NET_WM_NAME], xwm->atoms[UTF8_STRING], 8,
		strlen(wm_name), wm_name);
	xcb_change_property(xwm->xcb_conn, XCB_PROP_MODE_REPLACE,
		xwm->screen->root, xwm->atoms[NET_SUPPORTING_WM_CHECK],
		XCB_ATOM_WINDOW, 32, 1, &xwm->window);
	xcb_change_property(xwm->xcb_conn, XCB_PROP_MODE_REPLACE, xwm->window,
		xwm->atoms[NET_SUPPORTING_WM_CHECK], XCB_ATOM_WINDOW, 32, 1,
		&xwm->window);

	xcb_set_selection_owner(xwm->xcb_conn, xwm->window, xwm->atoms[WM_S0],
		XCB_CURRENT_TIME);
	xcb_set_selection_owner(xwm->xcb_conn, xwm->window,
		xwm->atoms[NET_WM_CM_S0], XCB_CURRENT_TIME);
}

// Holds focus when no window does.
static void create_no_focus_window(struct nx_xwm *xwm) {
	xwm->no_focus_window = xcb_generate_id(xwm->xcb_conn);

	uint32_t values[] = {
		1,
		XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_KEY_RELEASE |
			XCB_EVENT_MASK_FOCUS_CHANGE,
	};
	xcb_create_window(xwm->xcb_conn, XCB_COPY_FROM_PARENT,
		xwm->no_focus_window, xwm->screen->root, -100, -100, 1, 1, 0,
		XCB_WINDOW_CLASS_COPY_FROM_PARENT, XCB_COPY_FROM_PARENT,
		XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK, values);

	xcb_map_window(xwm->xcb_conn, xwm->no_focus_window);
}

void xwm_create_windows(struct nx_xwm *xwm) {
	create_wm_window(xwm);
	create_no_focus_window(xwm);
}
