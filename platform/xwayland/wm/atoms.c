#include <stdlib.h>
#include <string.h>
#include "platform/log/log.h"
#include "platform/xwayland/wm/xwm.h"

static const char utf8_mime_type[] = "text/plain;charset=utf-8";
static const char text_mime_type[] = "text/plain";

static const char *const atom_map[ATOM_LAST] = {
	[WL_SURFACE_ID] = "WL_SURFACE_ID",
	[WL_SURFACE_SERIAL] = "WL_SURFACE_SERIAL",
	[WM_DELETE_WINDOW] = "WM_DELETE_WINDOW",
	[WM_PROTOCOLS] = "WM_PROTOCOLS",
	[WM_HINTS] = "WM_HINTS",
	[WM_NORMAL_HINTS] = "WM_NORMAL_HINTS",
	[WM_SIZE_HINTS] = "WM_SIZE_HINTS",
	[UTF8_STRING] = "UTF8_STRING",
	[WM_S0] = "WM_S0",
	[NET_SUPPORTED] = "_NET_SUPPORTED",
	[NET_WM_CM_S0] = "_NET_WM_CM_S0",
	[NET_WM_NAME] = "_NET_WM_NAME",
	[NET_WM_STATE] = "_NET_WM_STATE",
	[NET_WM_WINDOW_TYPE] = "_NET_WM_WINDOW_TYPE",
	[WM_TAKE_FOCUS] = "WM_TAKE_FOCUS",
	[WINDOW] = "WINDOW",
	[NET_ACTIVE_WINDOW] = "_NET_ACTIVE_WINDOW",
	[NET_CLOSE_WINDOW] = "_NET_CLOSE_WINDOW",
	[NET_WM_MOVERESIZE] = "_NET_WM_MOVERESIZE",
	[NET_SUPPORTING_WM_CHECK] = "_NET_SUPPORTING_WM_CHECK",
	[NET_WM_STATE_FOCUSED] = "_NET_WM_STATE_FOCUSED",
	[NET_WM_STATE_MODAL] = "_NET_WM_STATE_MODAL",
	[NET_WM_STATE_FULLSCREEN] = "_NET_WM_STATE_FULLSCREEN",
	[NET_WM_STATE_MAXIMIZED_VERT] = "_NET_WM_STATE_MAXIMIZED_VERT",
	[NET_WM_STATE_MAXIMIZED_HORZ] = "_NET_WM_STATE_MAXIMIZED_HORZ",
	[NET_WM_STATE_HIDDEN] = "_NET_WM_STATE_HIDDEN",
	[NET_WM_STATE_STICKY] = "_NET_WM_STATE_STICKY",
	[NET_WM_STATE_SHADED] = "_NET_WM_STATE_SHADED",
	[NET_WM_STATE_SKIP_TASKBAR] = "_NET_WM_STATE_SKIP_TASKBAR",
	[NET_WM_STATE_SKIP_PAGER] = "_NET_WM_STATE_SKIP_PAGER",
	[NET_WM_STATE_ABOVE] = "_NET_WM_STATE_ABOVE",
	[NET_WM_STATE_BELOW] = "_NET_WM_STATE_BELOW",
	[NET_WM_STATE_DEMANDS_ATTENTION] = "_NET_WM_STATE_DEMANDS_ATTENTION",
	[NET_WM_PING] = "_NET_WM_PING",
	[WM_CHANGE_STATE] = "WM_CHANGE_STATE",
	[WM_STATE] = "WM_STATE",
	[CLIPBOARD] = "CLIPBOARD",
	[PRIMARY] = "PRIMARY",
	[WL_SELECTION] = "_WL_SELECTION",
	[TARGETS] = "TARGETS",
	[CLIPBOARD_MANAGER] = "CLIPBOARD_MANAGER",
	[INCR] = "INCR",
	[TEXT] = "TEXT",
	[TIMESTAMP] = "TIMESTAMP",
	[DELETE] = "DELETE",
	[NET_WM_WINDOW_TYPE_UTILITY] = "_NET_WM_WINDOW_TYPE_UTILITY",
	[NET_WM_WINDOW_TYPE_TOOLTIP] = "_NET_WM_WINDOW_TYPE_TOOLTIP",
	[NET_WM_WINDOW_TYPE_DND] = "_NET_WM_WINDOW_TYPE_DND",
	[NET_WM_WINDOW_TYPE_DROPDOWN_MENU] = "_NET_WM_WINDOW_TYPE_DROPDOWN_MENU",
	[NET_WM_WINDOW_TYPE_POPUP_MENU] = "_NET_WM_WINDOW_TYPE_POPUP_MENU",
	[NET_WM_WINDOW_TYPE_COMBO] = "_NET_WM_WINDOW_TYPE_COMBO",
	[NET_WM_WINDOW_TYPE_MENU] = "_NET_WM_WINDOW_TYPE_MENU",
	[NET_WM_WINDOW_TYPE_NOTIFICATION] = "_NET_WM_WINDOW_TYPE_NOTIFICATION",
	[NET_WM_WINDOW_TYPE_SPLASH] = "_NET_WM_WINDOW_TYPE_SPLASH",
	[NET_WM_WINDOW_TYPE_DESKTOP] = "_NET_WM_WINDOW_TYPE_DESKTOP",
	[NET_WM_WINDOW_TYPE_TOOLBAR] = "_NET_WM_WINDOW_TYPE_TOOLBAR",
	[NET_WM_WINDOW_TYPE_DIALOG] = "_NET_WM_WINDOW_TYPE_DIALOG",
	[DND_SELECTION] = "XdndSelection",
	[DND_AWARE] = "XdndAware",
	[DND_STATUS] = "XdndStatus",
	[DND_POSITION] = "XdndPosition",
	[DND_ENTER] = "XdndEnter",
	[DND_LEAVE] = "XdndLeave",
	[DND_DROP] = "XdndDrop",
	[DND_FINISHED] = "XdndFinished",
	[DND_TYPE_LIST] = "XdndTypeList",
	[DND_ACTION_MOVE] = "XdndActionMove",
	[DND_ACTION_COPY] = "XdndActionCopy",
	[DND_ACTION_ASK] = "XdndActionAsk",
	[DND_ACTION_PRIVATE] = "XdndActionPrivate",
	[NET_CLIENT_LIST] = "_NET_CLIENT_LIST",
	[NET_CLIENT_LIST_STACKING] = "_NET_CLIENT_LIST_STACKING",
};

bool xwm_intern_atoms(struct nx_xwm *xwm) {
	xcb_intern_atom_cookie_t cookies[ATOM_LAST];
	for (size_t i = 0; i < ATOM_LAST; i++) {
		cookies[i] = xcb_intern_atom(xwm->xcb_conn, 0,
			strlen(atom_map[i]), atom_map[i]);
	}

	for (size_t i = 0; i < ATOM_LAST; i++) {
		xcb_generic_error_t *error;
		xcb_intern_atom_reply_t *reply =
			xcb_intern_atom_reply(xwm->xcb_conn, cookies[i], &error);
		if (error) {
			nx_log(NX_ERROR, "could not resolve atom %s, x11 error code %d",
				atom_map[i], error->error_code);
			free(error);
			return false;
		}
		if (reply) {
			xwm->atoms[i] = reply->atom;
		}
		free(reply);
	}
	return true;
}

bool xwm_atoms_contains(const struct nx_xwm *xwm, const xcb_atom_t *atoms,
		size_t num_atoms, enum atom_name needle) {
	xcb_atom_t atom = xwm->atoms[needle];
	for (size_t i = 0; i < num_atoms; i++) {
		if (atoms[i] == atom) {
			return true;
		}
	}
	return false;
}

xcb_atom_t xwm_mime_type_to_atom(struct nx_xwm *xwm, const char *mime_type) {
	if (strcmp(mime_type, utf8_mime_type) == 0) {
		return xwm->atoms[UTF8_STRING];
	}
	if (strcmp(mime_type, text_mime_type) == 0) {
		return xwm->atoms[TEXT];
	}

	xcb_intern_atom_cookie_t cookie =
		xcb_intern_atom(xwm->xcb_conn, 0, strlen(mime_type), mime_type);
	xcb_intern_atom_reply_t *reply =
		xcb_intern_atom_reply(xwm->xcb_conn, cookie, NULL);
	if (reply == NULL) {
		return XCB_ATOM_NONE;
	}
	xcb_atom_t atom = reply->atom;
	free(reply);
	return atom;
}

static char *get_atom_name(struct nx_xwm *xwm, xcb_atom_t atom) {
	xcb_get_atom_name_cookie_t cookie = xcb_get_atom_name(xwm->xcb_conn, atom);
	xcb_get_atom_name_reply_t *reply =
		xcb_get_atom_name_reply(xwm->xcb_conn, cookie, NULL);
	if (reply == NULL) {
		return NULL;
	}
	char *name = strndup(xcb_get_atom_name_name(reply),
		xcb_get_atom_name_name_length(reply));
	free(reply);
	return name;
}

char *xwm_mime_type_from_atom(struct nx_xwm *xwm, xcb_atom_t atom) {
	if (atom == xwm->atoms[UTF8_STRING]) {
		return strdup(utf8_mime_type);
	}
	if (atom == xwm->atoms[TEXT]) {
		return strdup(text_mime_type);
	}
	return get_atom_name(xwm, atom);
}
