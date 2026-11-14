#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "platform/log/log.h"
#include "platform/xwayland/server/server.h"
#include "platform/xwayland/server/sockets.h"

static void safe_close(int fd) {
	if (fd >= 0) {
		close(fd);
	}
}

static void stop_listening(struct nx_xwayland_server *server) {
	wl_event_source_remove(server->x_fd_read_event[0]);
	wl_event_source_remove(server->x_fd_read_event[1]);
	server->x_fd_read_event[0] = server->x_fd_read_event[1] = NULL;
}

void xwayland_server_finish_process(struct nx_xwayland_server *server) {
	if (server->display == -1) {
		return;
	}

	if (server->x_fd_read_event[0]) {
		stop_listening(server);
	}
	if (server->client) {
		wl_list_remove(&server->client_destroy.link);
		wl_client_destroy(server->client);
	}
	if (server->pipe_source) {
		wl_event_source_remove(server->pipe_source);
	}

	safe_close(server->wl_fd[0]);
	safe_close(server->wl_fd[1]);
	safe_close(server->wm_fd[0]);
	safe_close(server->wm_fd[1]);
	memset(server, 0, offsetof(struct nx_xwayland_server, display));
	server->wl_fd[0] = server->wl_fd[1] = -1;
	server->wm_fd[0] = server->wm_fd[1] = -1;

	// Closed sockets make Xwayland exit.
}

void xwayland_server_finish_display(struct nx_xwayland_server *server) {
	wl_list_remove(&server->display_destroy.link);
	wl_list_init(&server->display_destroy.link);

	if (server->display == -1) {
		return;
	}

	safe_close(server->x_fd[0]);
	safe_close(server->x_fd[1]);
	server->x_fd[0] = server->x_fd[1] = -1;

	xwayland_unlink_display_sockets(server->display);
	server->display = -1;
	server->display_name[0] = '\0';
}

static int handle_x11_connection(int fd, uint32_t mask, void *data) {
	struct nx_xwayland_server *server = data;
	stop_listening(server);
	xwayland_server_spawn(server);
	return 0;
}

static bool listen_lazily(struct nx_xwayland_server *server) {
	struct wl_event_loop *loop = wl_display_get_event_loop(server->wl_display);

	server->x_fd_read_event[0] = wl_event_loop_add_fd(loop, server->x_fd[0],
		WL_EVENT_READABLE, handle_x11_connection, server);
	if (server->x_fd_read_event[0] == NULL) {
		return false;
	}

	server->x_fd_read_event[1] = wl_event_loop_add_fd(loop, server->x_fd[1],
		WL_EVENT_READABLE, handle_x11_connection, server);
	if (server->x_fd_read_event[1] == NULL) {
		wl_event_source_remove(server->x_fd_read_event[0]);
		server->x_fd_read_event[0] = NULL;
		return false;
	}

	return true;
}

static void handle_client_destroy(struct wl_listener *listener, void *data) {
	struct nx_xwayland_server *server =
		wl_container_of(listener, server, client_destroy);

	// The ready handler handles failures.
	if (server->pipe_source) {
		return;
	}

	// Already being destroyed.
	server->client = NULL;
	wl_list_remove(&server->client_destroy.link);

	xwayland_server_finish_process(server);

	nx_log(NX_INFO, "Restarting Xwayland (lazy)");
	listen_lazily(server);
}

static void handle_display_destroy(struct wl_listener *listener, void *data) {
	struct nx_xwayland_server *server =
		wl_container_of(listener, server, display_destroy);

	// Too late to destroy the client.
	if (server->client) {
		server->client = NULL;
		wl_list_remove(&server->client_destroy.link);
	}

	nx_xwayland_server_destroy(server);
}

static bool open_display(struct nx_xwayland_server *server,
		struct wl_display *wl_display) {
	server->display_destroy.notify = handle_display_destroy;
	wl_display_add_destroy_listener(wl_display, &server->display_destroy);

	server->display = xwayland_open_display_sockets(server->x_fd);
	if (server->display < 0) {
		xwayland_server_finish_display(server);
		return false;
	}

	snprintf(server->display_name, sizeof(server->display_name),
		":%d", server->display);
	return true;
}

void nx_xwayland_server_destroy(struct nx_xwayland_server *server) {
	if (!server) {
		return;
	}

	xwayland_server_finish_process(server);
	xwayland_server_finish_display(server);

	wl_signal_emit_mutable(&server->events.destroy, NULL);

	assert(wl_list_empty(&server->events.start.listener_list));
	assert(wl_list_empty(&server->events.ready.listener_list));
	assert(wl_list_empty(&server->events.destroy.listener_list));

	free(server);
}

struct nx_xwayland_server *nx_xwayland_server_create(
		struct wl_display *wl_display) {
	if (!getenv("NIXLY_XWAYLAND") && access(XWAYLAND_PATH, X_OK) != 0) {
		nx_log(NX_ERROR, "Cannot find Xwayland binary \"%s\"", XWAYLAND_PATH);
		return NULL;
	}

	struct nx_xwayland_server *server = calloc(1, sizeof(*server));
	if (!server) {
		return NULL;
	}

	server->wl_display = wl_display;
	server->x_fd[0] = server->x_fd[1] = -1;
	server->wl_fd[0] = server->wl_fd[1] = -1;
	server->wm_fd[0] = server->wm_fd[1] = -1;
	server->client_destroy.notify = handle_client_destroy;

	wl_signal_init(&server->events.start);
	wl_signal_init(&server->events.ready);
	wl_signal_init(&server->events.destroy);

	if (!open_display(server, wl_display)) {
		free(server);
		return NULL;
	}

	if (!listen_lazily(server)) {
		xwayland_server_finish_display(server);
		free(server);
		return NULL;
	}

	return server;
}
