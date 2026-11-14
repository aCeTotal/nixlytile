#include <assert.h>
#include <stdlib.h>
#include <xcb/res.h>
#include "platform/log/log.h"
#include "platform/xwayland/wm/xwm.h"

struct nx_xwayland_surface *xwm_lookup_surface(struct nx_xwm *xwm,
		xcb_window_t window_id) {
	struct nx_xwayland_surface *xsurface;
	wl_list_for_each(xsurface, &xwm->surfaces, link) {
		if (xsurface->window_id == window_id) {
			return xsurface;
		}
	}
	return NULL;
}

static int handle_ping_timeout(void *data) {
	struct nx_xwayland_surface *xsurface = data;
	wl_signal_emit_mutable(&xsurface->events.ping_timeout, NULL);
	xsurface->pinging = false;
	return 1;
}

static xcb_res_query_client_ids_cookie_t query_client_pid(struct nx_xwm *xwm,
		xcb_window_t window) {
	xcb_res_client_id_spec_t spec = {
		.client = window,
		.mask = XCB_RES_CLIENT_ID_MASK_LOCAL_CLIENT_PID,
	};
	return xcb_res_query_client_ids(xwm->xcb_conn, 1, &spec);
}

static void read_client_pid(struct nx_xwayland_surface *xsurface,
		xcb_res_query_client_ids_cookie_t cookie) {
	xcb_res_query_client_ids_reply_t *reply = xcb_res_query_client_ids_reply(
		xsurface->xwm->xcb_conn, cookie, NULL);
	if (reply == NULL) {
		return;
	}

	xcb_res_client_id_value_iterator_t iter =
		xcb_res_query_client_ids_ids_iterator(reply);
	for (; iter.rem > 0; xcb_res_client_id_value_next(&iter)) {
		if (!(iter.data->spec.mask & XCB_RES_CLIENT_ID_MASK_LOCAL_CLIENT_PID) ||
				xcb_res_client_id_value_value_length(iter.data) <= 0) {
			continue;
		}
		xsurface->pid = *xcb_res_client_id_value_value(iter.data);
		break;
	}
	free(reply);
}

static void init_signals(struct nx_xwayland_surface *xsurface) {
	wl_signal_init(&xsurface->events.destroy);
	wl_signal_init(&xsurface->events.request_configure);
	wl_signal_init(&xsurface->events.request_minimize);
	wl_signal_init(&xsurface->events.request_fullscreen);
	wl_signal_init(&xsurface->events.request_activate);
	wl_signal_init(&xsurface->events.associate);
	wl_signal_init(&xsurface->events.dissociate);
	wl_signal_init(&xsurface->events.set_title);
	wl_signal_init(&xsurface->events.set_hints);
	wl_signal_init(&xsurface->events.set_override_redirect);
	wl_signal_init(&xsurface->events.ping_timeout);
}

struct nx_xwayland_surface *xwm_surface_create(struct nx_xwm *xwm,
		const xcb_create_notify_event_t *ev) {
	struct nx_xwayland_surface *xsurface = calloc(1, sizeof(*xsurface));
	if (!xsurface) {
		nx_log(NX_ERROR, "Could not allocate xwayland surface");
		return NULL;
	}

	xcb_res_query_client_ids_cookie_t pid_cookie = { 0 };
	if (xwm->xres) {
		pid_cookie = query_client_pid(xwm, ev->window);
	}

	uint32_t event_mask = XCB_EVENT_MASK_FOCUS_CHANGE |
		XCB_EVENT_MASK_PROPERTY_CHANGE;
	xcb_change_window_attributes(xwm->xcb_conn, ev->window,
		XCB_CW_EVENT_MASK, &event_mask);

	xsurface->xwm = xwm;
	xsurface->window_id = ev->window;
	xsurface->x = ev->x;
	xsurface->y = ev->y;
	xsurface->width = ev->width;
	xsurface->height = ev->height;
	xsurface->override_redirect = ev->override_redirect;
	wl_list_init(&xsurface->children);
	wl_list_init(&xsurface->stack_link);
	wl_list_init(&xsurface->parent_link);
	wl_list_init(&xsurface->unpaired_link);
	init_signals(xsurface);

	struct wl_event_loop *loop =
		wl_display_get_event_loop(xwm->xwayland->wl_display);
	xsurface->ping_timer = wl_event_loop_add_timer(loop,
		handle_ping_timeout, xsurface);
	if (xsurface->ping_timer == NULL) {
		free(xsurface);
		nx_log(NX_ERROR, "Could not add timer to event loop");
		return NULL;
	}

	wl_list_insert(&xwm->surfaces, &xsurface->link);

	if (xwm->xres) {
		read_client_pid(xsurface, pid_cookie);
	}

	wl_signal_emit_mutable(&xwm->xwayland->events.new_surface, xsurface);
	return xsurface;
}

static void assert_no_listeners(struct nx_xwayland_surface *xsurface) {
	assert(wl_list_empty(&xsurface->events.destroy.listener_list));
	assert(wl_list_empty(&xsurface->events.request_configure.listener_list));
	assert(wl_list_empty(&xsurface->events.request_minimize.listener_list));
	assert(wl_list_empty(&xsurface->events.request_fullscreen.listener_list));
	assert(wl_list_empty(&xsurface->events.request_activate.listener_list));
	assert(wl_list_empty(&xsurface->events.associate.listener_list));
	assert(wl_list_empty(&xsurface->events.dissociate.listener_list));
	assert(wl_list_empty(&xsurface->events.set_title.listener_list));
	assert(wl_list_empty(&xsurface->events.set_hints.listener_list));
	assert(wl_list_empty(&xsurface->events.set_override_redirect.listener_list));
	assert(wl_list_empty(&xsurface->events.ping_timeout.listener_list));
}

static void orphan_children(struct nx_xwayland_surface *xsurface) {
	struct nx_xwayland_surface *child, *next;
	wl_list_for_each_safe(child, next, &xsurface->children, parent_link) {
		wl_list_remove(&child->parent_link);
		wl_list_init(&child->parent_link);
		child->parent = NULL;
	}
}

void xwm_surface_destroy(struct nx_xwayland_surface *xsurface) {
	struct nx_xwm *xwm = xsurface->xwm;

	xwm_surface_dissociate(xsurface);
	wl_signal_emit_mutable(&xsurface->events.destroy, NULL);
	assert_no_listeners(xsurface);

	if (xsurface == xwm->focus_surface) {
		xwm_surface_activate(xwm, NULL);
	}

	wl_list_remove(&xsurface->link);
	wl_list_remove(&xsurface->parent_link);
	orphan_children(xsurface);
	wl_list_remove(&xsurface->unpaired_link);
	wl_event_source_remove(xsurface->ping_timer);

	free(xsurface->wm_name);
	free(xsurface->net_wm_name);
	free(xsurface->class);
	free(xsurface->window_type);
	free(xsurface->protocols);
	free(xsurface->hints);
	free(xsurface->size_hints);
	free(xsurface);
}
