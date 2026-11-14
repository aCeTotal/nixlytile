#include <assert.h>
#include <stdlib.h>
#include "platform/input/seat/seat.h"
#include "platform/xwayland/server/server.h"
#include "platform/xwayland/shell.h"
#include "platform/xwayland/wm/xwm.h"

static void handle_server_destroy(struct wl_listener *listener, void *data) {
	struct nx_xwayland *xwayland =
		wl_container_of(listener, xwayland, server_destroy);
	// Already being destroyed.
	xwayland->server = NULL;
	nx_xwayland_destroy(xwayland);
}

static void handle_server_start(struct wl_listener *listener, void *data) {
	struct nx_xwayland *xwayland =
		wl_container_of(listener, xwayland, server_start);
	if (xwayland->shell_v1 != NULL) {
		nx_xwayland_shell_v1_set_client(xwayland->shell_v1,
			xwayland->server->client);
	}
}

static void handle_server_ready(struct wl_listener *listener, void *data) {
	struct nx_xwayland *xwayland =
		wl_container_of(listener, xwayland, server_ready);
	struct nx_xwayland_server *server = xwayland->server;

	assert(server->wm_fd[0] >= 0);
	xwayland->xwm = xwm_create(xwayland, server->wm_fd[0]);
	server->wm_fd[0] = -1; // owned by the XWM now
	if (!xwayland->xwm) {
		return;
	}

	wl_signal_emit_mutable(&xwayland->events.ready, NULL);
}

static void handle_shell_destroy(struct wl_listener *listener, void *data) {
	struct nx_xwayland *xwayland =
		wl_container_of(listener, xwayland, shell_destroy);
	xwayland->shell_v1 = NULL;
	// Stays removable in nx_xwayland_destroy.
	wl_list_remove(&xwayland->shell_destroy.link);
	wl_list_init(&xwayland->shell_destroy.link);
}

void nx_xwayland_destroy(struct nx_xwayland *xwayland) {
	if (!xwayland) {
		return;
	}

	assert(wl_list_empty(&xwayland->events.new_surface.listener_list));
	assert(wl_list_empty(&xwayland->events.ready.listener_list));

	wl_list_remove(&xwayland->server_destroy.link);
	wl_list_remove(&xwayland->server_start.link);
	wl_list_remove(&xwayland->server_ready.link);
	wl_list_remove(&xwayland->shell_destroy.link);

	nx_xwayland_set_seat(xwayland, NULL);
	nx_xwayland_server_destroy(xwayland->server);
	xwayland->server = NULL;
	nx_xwayland_shell_v1_destroy(xwayland->shell_v1);
	xwm_destroy(xwayland->xwm);
	free(xwayland);
}

static struct nx_xwayland *create(struct wl_display *wl_display,
		struct nx_compositor *compositor, struct nx_xwayland_server *server) {
	struct nx_xwayland *xwayland = calloc(1, sizeof(*xwayland));
	if (!xwayland) {
		return NULL;
	}

	xwayland->wl_display = wl_display;
	xwayland->compositor = compositor;
	xwayland->server = server;
	xwayland->display_name = server->display_name;
	wl_signal_init(&xwayland->events.new_surface);
	wl_signal_init(&xwayland->events.ready);

	xwayland->server_destroy.notify = handle_server_destroy;
	wl_signal_add(&server->events.destroy, &xwayland->server_destroy);
	xwayland->server_start.notify = handle_server_start;
	wl_signal_add(&server->events.start, &xwayland->server_start);
	xwayland->server_ready.notify = handle_server_ready;
	wl_signal_add(&server->events.ready, &xwayland->server_ready);
	return xwayland;
}

struct nx_xwayland *nx_xwayland_create(struct wl_display *wl_display,
		struct nx_compositor *compositor) {
	struct nx_xwayland_shell_v1 *shell_v1 = nx_xwayland_shell_v1_create(wl_display);
	if (shell_v1 == NULL) {
		return NULL;
	}

	struct nx_xwayland_server *server = nx_xwayland_server_create(wl_display);
	if (server == NULL) {
		nx_xwayland_shell_v1_destroy(shell_v1);
		return NULL;
	}

	struct nx_xwayland *xwayland = create(wl_display, compositor, server);
	if (xwayland == NULL) {
		nx_xwayland_server_destroy(server);
		nx_xwayland_shell_v1_destroy(shell_v1);
		return NULL;
	}

	xwayland->shell_v1 = shell_v1;
	xwayland->shell_destroy.notify = handle_shell_destroy;
	wl_signal_add(&shell_v1->events.destroy, &xwayland->shell_destroy);
	return xwayland;
}

void nx_xwayland_set_cursor(struct nx_xwayland *xwayland,
		struct nx_buffer *buffer, int32_t hotspot_x, int32_t hotspot_y) {
	if (xwayland->xwm != NULL) {
		xwm_set_cursor(xwayland->xwm, buffer, hotspot_x, hotspot_y);
	}
}

static void handle_seat_destroy(struct wl_listener *listener, void *data) {
	struct nx_xwayland *xwayland =
		wl_container_of(listener, xwayland, seat_destroy);
	nx_xwayland_set_seat(xwayland, NULL);
}

void nx_xwayland_set_seat(struct nx_xwayland *xwayland, struct nx_seat *seat) {
	if (xwayland->seat) {
		wl_list_remove(&xwayland->seat_destroy.link);
	}

	xwayland->seat = seat;
	if (xwayland->xwm) {
		xwm_set_seat(xwayland->xwm, seat);
	}
	if (seat == NULL) {
		return;
	}

	xwayland->seat_destroy.notify = handle_seat_destroy;
	wl_signal_add(&seat->events.destroy, &xwayland->seat_destroy);
}

xcb_connection_t *nx_xwayland_get_xwm_connection(struct nx_xwayland *xwayland) {
	return xwayland->xwm ? xwayland->xwm->xcb_conn : NULL;
}
