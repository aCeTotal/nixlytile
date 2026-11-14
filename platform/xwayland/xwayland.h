#ifndef NX_XWAYLAND_XWAYLAND_H
#define NX_XWAYLAND_XWAYLAND_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>
#include <xcb/xcb.h>
#include "platform/xwayland/surface.h"

struct nx_buffer;
struct nx_compositor;
struct nx_seat;
struct nx_xwayland_server;
struct nx_xwayland_shell_v1;

// Xwayland server, shell and window manager.
struct nx_xwayland {
	struct nx_xwayland_server *server;
	struct nx_xwm *xwm;
	struct nx_xwayland_shell_v1 *shell_v1;

	const char *display_name; // value for DISPLAY

	struct wl_display *wl_display;
	struct nx_compositor *compositor;
	struct nx_seat *seat;

	struct {
		struct wl_signal ready;
		struct wl_signal new_surface; // struct nx_xwayland_surface
	} events;

	struct wl_listener server_start;
	struct wl_listener server_ready;
	struct wl_listener server_destroy;
	struct wl_listener seat_destroy;
	struct wl_listener shell_destroy;
};

// Lazy: starts on first connect.
struct nx_xwayland *nx_xwayland_create(struct wl_display *wl_display,
	struct nx_compositor *compositor);

void nx_xwayland_destroy(struct nx_xwayland *xwayland);

void nx_xwayland_set_seat(struct nx_xwayland *xwayland, struct nx_seat *seat);

// Affects the running WM only.
void nx_xwayland_set_cursor(struct nx_xwayland *xwayland,
	struct nx_buffer *buffer, int32_t hotspot_x, int32_t hotspot_y);

// NULL before ready or after crash.
xcb_connection_t *nx_xwayland_get_xwm_connection(struct nx_xwayland *xwayland);

#endif
