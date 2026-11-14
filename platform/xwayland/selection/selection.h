#ifndef XWAYLAND_SELECTION_SELECTION_H
#define XWAYLAND_SELECTION_SELECTION_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>
#include <xcb/xcb.h>
#include <xcb/xfixes.h>

#define INCR_CHUNK_SIZE (64 * 1024)
#define XDND_VERSION 5

struct nx_data_source;
struct nx_drag;
struct nx_primary_selection_source;
struct nx_seat;
struct nx_xwm;

// One copy between X11 and Wayland.
struct nx_xwm_selection_transfer {
	struct nx_xwm_selection *selection;

	bool incr;
	bool flush_property_on_delete;
	bool property_set;
	struct wl_array source_data;
	int wl_client_fd;
	struct wl_event_source *event_source;
	struct wl_list link;

	// Wayland to X11
	xcb_selection_request_event_t request;

	// X11 to Wayland
	int property_start;
	xcb_get_property_reply_t *property_reply;
	xcb_window_t incoming_window;
};

struct nx_xwm_selection {
	struct nx_xwm *xwm;

	xcb_atom_t atom;
	xcb_window_t window;
	xcb_window_t owner;
	xcb_timestamp_t timestamp;

	struct wl_list incoming; // nx_xwm_selection_transfer.link
	struct wl_list outgoing; // nx_xwm_selection_transfer.link
};

void xwm_selection_init(struct nx_xwm_selection *selection,
	struct nx_xwm *xwm, xcb_atom_t atom);
void xwm_selection_finish(struct nx_xwm_selection *selection);
void xwm_set_seat(struct nx_xwm *xwm, struct nx_seat *seat);

// Nonzero if the event was consumed.
int xwm_handle_selection_event(struct nx_xwm *xwm, xcb_generic_event_t *event);
int xwm_handle_selection_client_message(struct nx_xwm *xwm,
	xcb_client_message_event_t *ev);
int xwm_handle_xfixes_selection_notify(struct nx_xwm *xwm,
	xcb_xfixes_selection_notify_event_t *event);
void xwm_handle_selection_notify(struct nx_xwm *xwm,
	xcb_selection_notify_event_t *event);
void xwm_handle_selection_request(struct nx_xwm *xwm,
	xcb_selection_request_event_t *req);
void xwm_handle_selection_destroy_notify(struct nx_xwm *xwm,
	xcb_destroy_notify_event_t *event);
struct nx_xwm_selection *xwm_get_selection(struct nx_xwm *xwm,
	xcb_atom_t selection_atom);

void xwm_selection_transfer_init(struct nx_xwm_selection_transfer *transfer,
	struct nx_xwm_selection *selection);
void xwm_selection_transfer_destroy(struct nx_xwm_selection_transfer *transfer);
void xwm_selection_transfer_remove_event_source(
	struct nx_xwm_selection_transfer *transfer);
void xwm_selection_transfer_close_wl_client_fd(
	struct nx_xwm_selection_transfer *transfer);
void xwm_selection_transfer_destroy_property_reply(
	struct nx_xwm_selection_transfer *transfer);

struct nx_xwm_selection_transfer *xwm_selection_find_incoming_transfer_by_window(
	struct nx_xwm_selection *selection, xcb_window_t window);
void xwm_selection_receive(struct nx_xwm_selection *selection,
	xcb_atom_t target, int fd);
void xwm_get_incr_chunk(struct nx_xwm_selection_transfer *transfer);
void xwm_selection_get_targets(struct nx_xwm_selection *selection);
bool xwm_data_source_is_xwayland(struct nx_data_source *source);
bool xwm_primary_selection_source_is_xwayland(
	struct nx_primary_selection_source *source);

void xwm_selection_send_notify(struct nx_xwm *xwm,
	xcb_selection_request_event_t *req, bool success);
void xwm_selection_transfer_start_outgoing(
	struct nx_xwm_selection_transfer *transfer);
void xwm_selection_transfer_destroy_outgoing(
	struct nx_xwm_selection_transfer *transfer);
void xwm_send_incr_chunk(struct nx_xwm_selection_transfer *transfer);

void xwm_seat_handle_start_drag(struct nx_xwm *xwm, struct nx_drag *drag);
void xwm_seat_unlink_drag_handlers(struct nx_xwm *xwm);
void xwm_dnd_send_enter(struct nx_xwm *xwm);
void xwm_dnd_send_position(struct nx_xwm *xwm, uint32_t time, int16_t x,
	int16_t y);
void xwm_dnd_send_drop(struct nx_xwm *xwm, uint32_t time);
void xwm_dnd_send_leave(struct nx_xwm *xwm);

#endif
