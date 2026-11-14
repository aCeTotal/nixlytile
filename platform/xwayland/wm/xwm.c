#include <stdlib.h>
#include <unistd.h>
#include "platform/clipboard/data_device/data_device.h"
#include "platform/clipboard/primary/primary_selection.h"
#include "platform/input/seat/seat.h"
#include "platform/log/log.h"
#include "platform/xwayland/wm/xwm.h"

// Drops X11-owned Wayland selections.
static void release_seat(struct nx_xwm *xwm) {
	struct nx_seat *seat = xwm->seat;
	struct wl_display *display = xwm->xwayland->wl_display;

	if (seat->selection_source &&
			xwm_data_source_is_xwayland(seat->selection_source)) {
		nx_seat_set_selection(seat, NULL, wl_display_next_serial(display));
	}
	if (seat->primary_selection_source &&
			xwm_primary_selection_source_is_xwayland(
				seat->primary_selection_source)) {
		nx_seat_set_primary_selection(seat, NULL,
			wl_display_next_serial(display));
	}

	nx_xwayland_set_seat(xwm->xwayland, NULL);
}

void xwm_destroy(struct nx_xwm *xwm) {
	if (!xwm) {
		return;
	}

	xwm_selection_finish(&xwm->clipboard_selection);
	xwm_selection_finish(&xwm->primary_selection);
	xwm_selection_finish(&xwm->dnd_selection);

	xwm_seat_unlink_drag_handlers(xwm);

	if (xwm->seat) {
		release_seat(xwm);
	}

	if (xwm->cursor) {
		xcb_free_cursor(xwm->xcb_conn, xwm->cursor);
	}
	if (xwm->no_focus_window) {
		xcb_destroy_window(xwm->xcb_conn, xwm->no_focus_window);
	}
	if (xwm->window) {
		xcb_destroy_window(xwm->xcb_conn, xwm->window);
	}
	if (xwm->event_source) {
		wl_event_source_remove(xwm->event_source);
	}
	if (xwm->errors_context) {
		xcb_errors_context_free(xwm->errors_context);
	}

	struct nx_xwayland_surface *xsurface, *tmp;
	wl_list_for_each_safe(xsurface, tmp, &xwm->surfaces, link) {
		xwm_surface_destroy(xsurface);
	}
	wl_list_remove(&xwm->compositor_new_surface.link);
	wl_list_remove(&xwm->compositor_destroy.link);
	wl_list_remove(&xwm->shell_v1_new_surface.link);
	wl_list_remove(&xwm->shell_v1_destroy.link);
	xcb_disconnect(xwm->xcb_conn);

	xwm->xwayland->xwm = NULL;
	free(xwm);
}

struct nx_xwm *xwm_create(struct nx_xwayland *xwayland, int wm_fd) {
	struct nx_xwm *xwm = calloc(1, sizeof(*xwm));
	if (xwm == NULL) {
		close(wm_fd);
		return NULL;
	}

	xwm->xwayland = xwayland;
	wl_list_init(&xwm->surfaces);
	wl_list_init(&xwm->surfaces_in_stack_order);
	wl_list_init(&xwm->unpaired_surfaces);
	wl_list_init(&xwm->seat_drag_source_destroy.link);
	wl_list_init(&xwm->drag_focus_destroy.link);
	wl_list_init(&xwm->drop_focus_destroy.link);

	// Takes wm_fd even on failure.
	xwm->xcb_conn = xcb_connect_to_fd(wm_fd, NULL);
	int rc = xcb_connection_has_error(xwm->xcb_conn);
	if (rc) {
		nx_log(NX_ERROR, "xcb connect failed: %d", rc);
		xcb_disconnect(xwm->xcb_conn);
		free(xwm);
		return NULL;
	}

	if (xcb_errors_context_new(xwm->xcb_conn, &xwm->errors_context)) {
		nx_log(NX_ERROR, "Could not allocate error context");
		xwm_destroy(xwm);
		return NULL;
	}

	xwm->screen = xcb_setup_roots_iterator(xcb_get_setup(xwm->xcb_conn)).data;

	struct wl_event_loop *loop = wl_display_get_event_loop(xwayland->wl_display);
	xwm->event_source = wl_event_loop_add_fd(loop, wm_fd,
		WL_EVENT_READABLE, xwm_handle_x11_events, xwm);
	wl_event_source_check(xwm->event_source);

	xwm_query_resources(xwm);
	xwm_find_cursor_format(xwm);
	xwm_manage_root(xwm);
	xwm_set_net_active_window(xwm, XCB_WINDOW_NONE);

	xwm_selection_init(&xwm->clipboard_selection, xwm, xwm->atoms[CLIPBOARD]);
	xwm_selection_init(&xwm->primary_selection, xwm, xwm->atoms[PRIMARY]);
	xwm_selection_init(&xwm->dnd_selection, xwm, xwm->atoms[DND_SELECTION]);

	xwm_watch_new_surfaces(xwm);
	xwm_create_windows(xwm);
	xcb_flush(xwm->xcb_conn);

	return xwm;
}
