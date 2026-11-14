#include <inttypes.h>
#include "platform/clipboard/data_device/data_device.h"
#include "platform/input/seat/seat.h"
#include "platform/log/log.h"
#include "platform/xwayland/wm/xwm.h"

static enum wl_data_device_manager_dnd_action action_from_atom(
		struct nx_xwm *xwm, xcb_atom_t atom) {
	if (atom == xwm->atoms[DND_ACTION_COPY] ||
			atom == xwm->atoms[DND_ACTION_PRIVATE]) {
		return WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY;
	}
	if (atom == xwm->atoms[DND_ACTION_MOVE]) {
		return WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE;
	}
	if (atom == xwm->atoms[DND_ACTION_ASK]) {
		return WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK;
	}
	return WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE;
}

// Target accepted or refused.
static void handle_status(struct nx_xwm *xwm, xcb_client_message_event_t *ev) {
	if (xwm->drag == NULL) {
		nx_log(NX_DEBUG, "ignoring XdndStatus client message because "
			"there's no drag");
		return;
	}

	xcb_window_t target_window = ev->data.data32[0];
	bool accepted = ev->data.data32[1] & 1;
	if (xwm->drag_focus == NULL ||
			target_window != xwm->drag_focus->window_id) {
		nx_log(NX_DEBUG, "ignoring XdndStatus client message because "
			"it doesn't match the current drag focus window ID");
		return;
	}

	enum wl_data_device_manager_dnd_action action =
		action_from_atom(xwm, ev->data.data32[4]);
	xwm->drag->source->accepted = accepted;
	nx_data_source_dnd_action(xwm->drag->source, action);

	nx_log(NX_DEBUG, "DND_STATUS window=%" PRIu32 " accepted=%d action=%d",
		target_window, accepted, action);
}

// Arrives after the drag ended.
static void handle_finished(struct nx_xwm *xwm, xcb_client_message_event_t *ev) {
	if (xwm->seat == NULL || xwm->seat->drag_source == NULL ||
			xwm->drag != NULL) {
		nx_log(NX_DEBUG, "ignoring XdndFinished client message because "
			"there's no finished drag");
		return;
	}

	xcb_window_t target_window = ev->data.data32[0];
	if (xwm->drop_focus == NULL ||
			target_window != xwm->drop_focus->window_id) {
		nx_log(NX_DEBUG, "ignoring XdndFinished client message because "
			"it doesn't match the finished drop focus window ID");
		return;
	}

	nx_data_source_dnd_finish(xwm->seat->drag_source);
	nx_log(NX_DEBUG, "DND_FINISH window=%" PRIu32, target_window);
}

int xwm_handle_selection_client_message(struct nx_xwm *xwm,
		xcb_client_message_event_t *ev) {
	if (ev->type == xwm->atoms[DND_STATUS]) {
		handle_status(xwm, ev);
		return 1;
	}
	if (ev->type == xwm->atoms[DND_FINISHED]) {
		handle_finished(xwm, ev);
		return 1;
	}
	return 0;
}

static void set_drag_focus(struct nx_xwm *xwm,
		struct nx_xwayland_surface *focus);

static void handle_drag_focus_destroy(struct wl_listener *listener, void *data) {
	struct nx_xwm *xwm = wl_container_of(listener, xwm, drag_focus_destroy);
	set_drag_focus(xwm, NULL);
}

static void handle_drop_focus_destroy(struct wl_listener *listener, void *data) {
	struct nx_xwm *xwm = wl_container_of(listener, xwm, drop_focus_destroy);
	wl_list_remove(&xwm->drop_focus_destroy.link);
	wl_list_init(&xwm->drop_focus_destroy.link);
	xwm->drop_focus = NULL;
}

static void set_drag_focus(struct nx_xwm *xwm,
		struct nx_xwayland_surface *focus) {
	if (focus == xwm->drag_focus) {
		return;
	}

	if (xwm->drag_focus != NULL) {
		nx_data_source_dnd_action(xwm->drag->source,
			WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE);
		xwm_dnd_send_leave(xwm);
	}

	wl_list_remove(&xwm->drag_focus_destroy.link);
	wl_list_init(&xwm->drag_focus_destroy.link);

	xwm->drag_focus = focus;
	if (focus == NULL) {
		return;
	}

	xwm->drag_focus_destroy.notify = handle_drag_focus_destroy;
	wl_signal_add(&focus->events.destroy, &xwm->drag_focus_destroy);
	xwm_dnd_send_enter(xwm);
}

static void handle_drag_focus(struct wl_listener *listener, void *data) {
	struct nx_xwm *xwm = wl_container_of(listener, xwm, seat_drag_focus);
	struct nx_drag *drag = data;

	struct nx_xwayland_surface *focus = NULL;
	if (drag->focus != NULL) {
		focus = nx_xwayland_surface_try_from_nx_surface(drag->focus);
	}
	set_drag_focus(xwm, focus);
}

static void handle_drag_motion(struct wl_listener *listener, void *data) {
	struct nx_xwm *xwm = wl_container_of(listener, xwm, seat_drag_motion);
	struct nx_drag_motion_event *event = data;
	struct nx_xwayland_surface *focus = xwm->drag_focus;
	if (focus == NULL) {
		return;
	}

	xwm_dnd_send_position(xwm, event->time, focus->x + (int16_t)event->sx,
		focus->y + (int16_t)event->sy);
}

static void handle_drag_drop(struct wl_listener *listener, void *data) {
	struct nx_xwm *xwm = wl_container_of(listener, xwm, seat_drag_drop);
	struct nx_drag_drop_event *event = data;
	if (xwm->drag_focus == NULL) {
		return;
	}

	nx_log(NX_DEBUG, "Wayland drag dropped over an Xwayland window");

	xwm->drop_focus = xwm->drag_focus;
	xwm->drop_focus_destroy.notify = handle_drop_focus_destroy;
	wl_list_remove(&xwm->drop_focus_destroy.link);
	wl_signal_add(&xwm->drop_focus->events.destroy, &xwm->drop_focus_destroy);

	xwm_dnd_send_drop(xwm, event->time);
}

static void unlink_drag(struct nx_xwm *xwm) {
	wl_list_remove(&xwm->seat_drag_focus.link);
	wl_list_remove(&xwm->seat_drag_motion.link);
	wl_list_remove(&xwm->seat_drag_drop.link);
	wl_list_remove(&xwm->seat_drag_destroy.link);
}

static void handle_drag_destroy(struct wl_listener *listener, void *data) {
	struct nx_xwm *xwm = wl_container_of(listener, xwm, seat_drag_destroy);

	// Target still reads the source.
	if (xwm->drag_focus != NULL && !xwm->drag->source->accepted) {
		nx_log(NX_DEBUG, "Wayland drag cancelled over an Xwayland window");
		xwm_dnd_send_leave(xwm);
	}

	unlink_drag(xwm);
	xwm->drag = NULL;
}

static void handle_drag_source_destroy(struct wl_listener *listener,
		void *data) {
	struct nx_xwm *xwm = wl_container_of(listener, xwm, seat_drag_source_destroy);

	wl_list_remove(&xwm->seat_drag_source_destroy.link);
	wl_list_init(&xwm->seat_drag_source_destroy.link);
	wl_list_remove(&xwm->drag_focus_destroy.link);
	wl_list_init(&xwm->drag_focus_destroy.link);
	xwm->drag_focus = NULL;

	wl_list_remove(&xwm->drop_focus_destroy.link);
	wl_list_init(&xwm->drop_focus_destroy.link);
	xwm->drop_focus = NULL;
}

void xwm_seat_handle_start_drag(struct nx_xwm *xwm, struct nx_drag *drag) {
	wl_list_remove(&xwm->drag_focus_destroy.link);
	wl_list_init(&xwm->drag_focus_destroy.link);
	wl_list_remove(&xwm->drop_focus_destroy.link);
	wl_list_init(&xwm->drop_focus_destroy.link);

	xwm->drag = drag;
	xwm->drag_focus = NULL;
	xwm->drop_focus = NULL;
	if (drag == NULL) {
		return;
	}

	xwm->seat_drag_focus.notify = handle_drag_focus;
	wl_signal_add(&drag->events.focus, &xwm->seat_drag_focus);
	xwm->seat_drag_motion.notify = handle_drag_motion;
	wl_signal_add(&drag->events.motion, &xwm->seat_drag_motion);
	xwm->seat_drag_drop.notify = handle_drag_drop;
	wl_signal_add(&drag->events.drop, &xwm->seat_drag_drop);
	xwm->seat_drag_destroy.notify = handle_drag_destroy;
	wl_signal_add(&drag->events.destroy, &xwm->seat_drag_destroy);
	xwm->seat_drag_source_destroy.notify = handle_drag_source_destroy;
	wl_signal_add(&drag->source->events.destroy, &xwm->seat_drag_source_destroy);
}

void xwm_seat_unlink_drag_handlers(struct nx_xwm *xwm) {
	wl_list_remove(&xwm->seat_drag_source_destroy.link);
	wl_list_remove(&xwm->drag_focus_destroy.link);
	wl_list_remove(&xwm->drop_focus_destroy.link);
	if (xwm->drag) {
		unlink_drag(xwm);
	}
}
