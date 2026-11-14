#ifndef XWAYLAND_WM_XWM_H
#define XWAYLAND_WM_XWM_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>
#include <xcb/render.h>
#include <xcb/xcb.h>
#include <xcb/xcb_errors.h>
#include "platform/xwayland/selection/selection.h"
#include "platform/xwayland/wm/atoms.h"
#include "platform/xwayland/xwayland.h"

// Strips the synthetic-event bit.
#define XCB_EVENT_RESPONSE_TYPE_MASK 0x7f

struct nx_xwm {
	struct nx_xwayland *xwayland;
	struct wl_event_source *event_source;
	struct nx_seat *seat;

	xcb_atom_t atoms[ATOM_LAST];
	xcb_connection_t *xcb_conn;
	xcb_screen_t *screen;
	xcb_window_t window;
	xcb_window_t no_focus_window;
	xcb_render_pictformat_t render_format_id;
	xcb_cursor_t cursor;

	struct nx_xwm_selection clipboard_selection;
	struct nx_xwm_selection primary_selection;
	struct nx_xwm_selection dnd_selection;

	struct nx_xwayland_surface *focus_surface;

	struct wl_list surfaces; // nx_xwayland_surface.link, creation order
	struct wl_list surfaces_in_stack_order; // nx_xwayland_surface.stack_link, bottom first
	struct wl_list unpaired_surfaces; // nx_xwayland_surface.unpaired_link

	struct nx_drag *drag;
	struct nx_xwayland_surface *drag_focus;
	struct nx_xwayland_surface *drop_focus;

	const xcb_query_extension_reply_t *xfixes;
	const xcb_query_extension_reply_t *xres;
	uint32_t xfixes_major_version;
	xcb_errors_context_t *errors_context;
	unsigned int last_focus_seq;

	struct wl_listener compositor_new_surface;
	struct wl_listener compositor_destroy;
	struct wl_listener shell_v1_new_surface;
	struct wl_listener shell_v1_destroy;
	struct wl_listener seat_set_selection;
	struct wl_listener seat_set_primary_selection;
	struct wl_listener seat_start_drag;
	struct wl_listener seat_drag_focus;
	struct wl_listener seat_drag_motion;
	struct wl_listener seat_drag_drop;
	struct wl_listener seat_drag_destroy;
	struct wl_listener seat_drag_source_destroy;
	struct wl_listener drag_focus_destroy;
	struct wl_listener drop_focus_destroy;
};

// Owns wm_fd, even on failure.
struct nx_xwm *xwm_create(struct nx_xwayland *xwayland, int wm_fd);
void xwm_destroy(struct nx_xwm *xwm);

void xwm_query_resources(struct nx_xwm *xwm);
void xwm_manage_root(struct nx_xwm *xwm);
void xwm_create_windows(struct nx_xwm *xwm);

void xwm_find_cursor_format(struct nx_xwm *xwm);
void xwm_set_cursor(struct nx_xwm *xwm, struct nx_buffer *buffer,
	int32_t hotspot_x, int32_t hotspot_y);

int xwm_handle_x11_events(int fd, uint32_t mask, void *data);
void xwm_schedule_flush(struct nx_xwm *xwm);
// Pads events to 32 bytes.
void xwm_send_event(struct nx_xwm *xwm, xcb_window_t destination,
	uint32_t event_mask, const void *event, size_t length);

void xwm_handle_create_notify(struct nx_xwm *xwm, xcb_create_notify_event_t *ev);
void xwm_handle_destroy_notify(struct nx_xwm *xwm, xcb_destroy_notify_event_t *ev);
void xwm_handle_configure_request(struct nx_xwm *xwm,
	xcb_configure_request_event_t *ev);
void xwm_handle_configure_notify(struct nx_xwm *xwm,
	xcb_configure_notify_event_t *ev);
void xwm_handle_map_request(struct nx_xwm *xwm, xcb_map_request_event_t *ev);
void xwm_handle_map_notify(struct nx_xwm *xwm, xcb_map_notify_event_t *ev);
void xwm_handle_unmap_notify(struct nx_xwm *xwm, xcb_unmap_notify_event_t *ev);
void xwm_handle_property_notify(struct nx_xwm *xwm,
	xcb_property_notify_event_t *ev);
void xwm_handle_client_message(struct nx_xwm *xwm,
	xcb_client_message_event_t *ev);
void xwm_handle_focus_in(struct nx_xwm *xwm, xcb_focus_in_event_t *ev);

void xwm_set_net_active_window(struct nx_xwm *xwm, xcb_window_t window);
void xwm_surface_activate(struct nx_xwm *xwm,
	struct nx_xwayland_surface *xsurface);

void xwm_update_client_list(struct nx_xwm *xwm);
void xwm_update_client_list_stacking(struct nx_xwm *xwm);
void xwm_update_override_redirect(struct nx_xwayland_surface *xsurface,
	bool override_redirect);

struct nx_xwayland_surface *xwm_lookup_surface(struct nx_xwm *xwm,
	xcb_window_t window_id);
struct nx_xwayland_surface *xwm_surface_create(struct nx_xwm *xwm,
	const xcb_create_notify_event_t *ev);
void xwm_surface_destroy(struct nx_xwayland_surface *xsurface);

void xwm_watch_new_surfaces(struct nx_xwm *xwm);
void xwm_surface_dissociate(struct nx_xwayland_surface *xsurface);
void xwm_handle_surface_id_message(struct nx_xwm *xwm,
	xcb_client_message_event_t *ev);
void xwm_handle_surface_serial_message(struct nx_xwm *xwm,
	xcb_client_message_event_t *ev);

void xwm_read_initial_properties(struct nx_xwayland_surface *xsurface);
void xwm_read_hints(struct nx_xwayland_surface *xsurface,
	xcb_get_property_reply_t *reply);
void xwm_read_normal_hints(struct nx_xwayland_surface *xsurface,
	xcb_get_property_reply_t *reply);
void xwm_read_net_wm_state(struct nx_xwayland_surface *xsurface,
	xcb_get_property_reply_t *reply);

void xwm_surface_update_net_wm_state(struct nx_xwayland_surface *xsurface);
void xwm_surface_set_withdrawn(struct nx_xwayland_surface *xsurface,
	bool withdrawn);
void xwm_handle_net_wm_state_message(struct nx_xwm *xwm,
	xcb_client_message_event_t *ev);

void xwm_send_wm_message(struct nx_xwayland_surface *xsurface,
	const xcb_client_message_data_t *data, uint32_t event_mask);

#endif
