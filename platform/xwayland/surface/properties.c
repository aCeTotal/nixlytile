#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include "platform/log/log.h"
#include "platform/xwayland/wm/xwm.h"

#define PROPERTY_LENGTH 2048 // in 32-bit units

typedef void (*property_reader)(struct nx_xwayland_surface *xsurface,
	xcb_get_property_reply_t *reply);

static bool is_text(struct nx_xwm *xwm, const xcb_get_property_reply_t *reply) {
	return reply->type == XCB_ATOM_STRING ||
		reply->type == xwm->atoms[UTF8_STRING] ||
		reply->type == XCB_ATOM_NONE;
}

// WM_CLASS holds instance and class.
static void read_class(struct nx_xwayland_surface *xsurface,
		xcb_get_property_reply_t *reply) {
	if (!is_text(xsurface->xwm, reply)) {
		nx_log(NX_DEBUG, "Invalid WM_CLASS property type");
		return;
	}

	size_t len = xcb_get_property_value_length(reply);
	const char *class = xcb_get_property_value(reply);
	size_t instance_len = strnlen(class, len);
	if (instance_len < len) {
		class += instance_len + 1;
		len -= instance_len + 1;
	}

	free(xsurface->class);
	xsurface->class = len > 0 ? strndup(class, len) : NULL;
}

static void read_title(struct nx_xwayland_surface *xsurface, char **name,
		xcb_get_property_reply_t *reply) {
	if (!is_text(xsurface->xwm, reply)) {
		nx_log(NX_DEBUG, "Invalid WM_NAME/NET_WM_NAME property type");
		return;
	}

	size_t len = xcb_get_property_value_length(reply);
	free(*name);
	*name = len > 0 ? strndup(xcb_get_property_value(reply), len) : NULL;

	// _NET_WM_NAME wins over WM_NAME.
	xsurface->title = xsurface->net_wm_name != NULL ?
		xsurface->net_wm_name : xsurface->wm_name;
	wl_signal_emit_mutable(&xsurface->events.set_title, NULL);
}

static void read_wm_name(struct nx_xwayland_surface *xsurface,
		xcb_get_property_reply_t *reply) {
	read_title(xsurface, &xsurface->wm_name, reply);
}

static void read_net_wm_name(struct nx_xwayland_surface *xsurface,
		xcb_get_property_reply_t *reply) {
	read_title(xsurface, &xsurface->net_wm_name, reply);
}

static bool has_parent(struct nx_xwayland_surface *parent,
		struct nx_xwayland_surface *child) {
	for (; parent; parent = parent->parent) {
		if (child == parent) {
			return true;
		}
	}
	return false;
}

static void set_parent(struct nx_xwayland_surface *xsurface,
		struct nx_xwayland_surface *parent) {
	xsurface->parent = parent;
	wl_list_remove(&xsurface->parent_link);
	if (parent == NULL) {
		wl_list_init(&xsurface->parent_link);
		return;
	}
	wl_list_insert(&parent->children, &xsurface->parent_link);
}

static void read_parent(struct nx_xwayland_surface *xsurface,
		xcb_get_property_reply_t *reply) {
	if (reply->type == XCB_ATOM_NONE) {
		set_parent(xsurface, NULL);
		return;
	}
	if (reply->type != XCB_ATOM_WINDOW) {
		nx_log(NX_DEBUG, "Invalid WM_TRANSIENT_FOR property type");
		return;
	}
	if (xcb_get_property_value_length(reply) != sizeof(xcb_window_t)) {
		nx_log(NX_DEBUG, "Invalid WM_TRANSIENT_FOR property length");
		return;
	}

	const xcb_window_t *xid = xcb_get_property_value(reply);
	struct nx_xwayland_surface *parent = xwm_lookup_surface(xsurface->xwm, *xid);
	if (has_parent(parent, xsurface)) {
		nx_log(NX_INFO, "%p with %p would create a loop",
			(void *)xsurface, (void *)parent);
		parent = xsurface->parent;
	}
	set_parent(xsurface, parent);
}

static void copy_atoms(xcb_get_property_reply_t *reply, xcb_atom_t **atoms,
		size_t *len) {
	size_t count = xcb_get_property_value_length(reply) / sizeof(xcb_atom_t);
	free(*atoms);
	*atoms = NULL;
	if (count == 0) {
		*len = 0;
		return;
	}

	*atoms = malloc(count * sizeof(xcb_atom_t));
	if (*atoms == NULL) {
		return;
	}
	memcpy(*atoms, xcb_get_property_value(reply), count * sizeof(xcb_atom_t));
	*len = count;
}

static void read_window_type(struct nx_xwayland_surface *xsurface,
		xcb_get_property_reply_t *reply) {
	if (reply->type != XCB_ATOM_ATOM && reply->type != XCB_ATOM_NONE) {
		nx_log(NX_DEBUG, "Invalid NET_WM_WINDOW_TYPE property type");
		return;
	}
	copy_atoms(reply, &xsurface->window_type, &xsurface->window_type_len);
}

static void read_protocols(struct nx_xwayland_surface *xsurface,
		xcb_get_property_reply_t *reply) {
	if (reply->type != XCB_ATOM_ATOM && reply->type != XCB_ATOM_NONE) {
		nx_log(NX_DEBUG, "Invalid WM_PROTOCOLS property type");
		return;
	}
	copy_atoms(reply, &xsurface->protocols, &xsurface->protocols_len);
}

static property_reader find_reader(struct nx_xwm *xwm, xcb_atom_t atom) {
	static const struct {
		enum atom_name atom;
		property_reader reader;
	} readers[] = {
		{ NET_WM_NAME, read_net_wm_name },
		{ NET_WM_WINDOW_TYPE, read_window_type },
		{ WM_PROTOCOLS, read_protocols },
		{ NET_WM_STATE, xwm_read_net_wm_state },
		{ WM_HINTS, xwm_read_hints },
		{ WM_NORMAL_HINTS, xwm_read_normal_hints },
	};

	switch (atom) {
	case XCB_ATOM_WM_CLASS:
		return read_class;
	case XCB_ATOM_WM_NAME:
		return read_wm_name;
	case XCB_ATOM_WM_TRANSIENT_FOR:
		return read_parent;
	}
	for (size_t i = 0; i < sizeof(readers) / sizeof(readers[0]); i++) {
		if (xwm->atoms[readers[i].atom] == atom) {
			return readers[i].reader;
		}
	}
	return NULL;
}

static xcb_get_property_cookie_t get_property(struct nx_xwm *xwm,
		xcb_window_t window, xcb_atom_t atom) {
	return xcb_get_property(xwm->xcb_conn, 0, window, atom, XCB_ATOM_ANY,
		0, PROPERTY_LENGTH);
}

static void apply_reply(struct nx_xwayland_surface *xsurface,
		property_reader reader, xcb_get_property_cookie_t cookie) {
	xcb_get_property_reply_t *reply =
		xcb_get_property_reply(xsurface->xwm->xcb_conn, cookie, NULL);
	if (reply == NULL) {
		nx_log(NX_ERROR, "Failed to get window property");
		return;
	}
	reader(xsurface, reply);
	free(reply);
}

void xwm_read_initial_properties(struct nx_xwayland_surface *xsurface) {
	struct nx_xwm *xwm = xsurface->xwm;
	const xcb_atom_t props[] = {
		XCB_ATOM_WM_CLASS,
		XCB_ATOM_WM_NAME,
		XCB_ATOM_WM_TRANSIENT_FOR,
		xwm->atoms[WM_PROTOCOLS],
		xwm->atoms[WM_HINTS],
		xwm->atoms[WM_NORMAL_HINTS],
		xwm->atoms[NET_WM_STATE],
		xwm->atoms[NET_WM_WINDOW_TYPE],
		xwm->atoms[NET_WM_NAME],
	};
	size_t count = sizeof(props) / sizeof(props[0]);

	// Pipeline all requests first.
	xcb_get_property_cookie_t cookies[sizeof(props) / sizeof(props[0])];
	for (size_t i = 0; i < count; i++) {
		cookies[i] = get_property(xwm, xsurface->window_id, props[i]);
	}
	for (size_t i = 0; i < count; i++) {
		apply_reply(xsurface, find_reader(xwm, props[i]), cookies[i]);
	}
}

// Unknown atoms skip the round-trip.
void xwm_handle_property_notify(struct nx_xwm *xwm,
		xcb_property_notify_event_t *ev) {
	struct nx_xwayland_surface *xsurface = xwm_lookup_surface(xwm, ev->window);
	if (xsurface == NULL) {
		return;
	}

	property_reader reader = find_reader(xwm, ev->atom);
	if (reader == NULL) {
		nx_log(NX_DEBUG, "unhandled X11 property %" PRIu32 " for window %" PRIu32,
			ev->atom, xsurface->window_id);
		return;
	}
	apply_reply(xsurface, reader, get_property(xwm, xsurface->window_id, ev->atom));
}
