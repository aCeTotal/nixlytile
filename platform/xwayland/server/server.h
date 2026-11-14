#ifndef XWAYLAND_SERVER_SERVER_H
#define XWAYLAND_SERVER_SERVER_H

#include <sys/types.h>
#include <wayland-server-core.h>

// The Xwayland process, started lazily.
struct nx_xwayland_server {
	pid_t pid;
	struct wl_client *client;
	struct wl_event_source *pipe_source;
	int wm_fd[2], wl_fd[2];

	// Below survives restarts.
	int display;
	char display_name[16];
	int x_fd[2];
	struct wl_event_source *x_fd_read_event[2];

	struct wl_display *wl_display;

	struct {
		struct wl_signal start;
		struct wl_signal ready;
		struct wl_signal destroy;
	} events;

	struct wl_listener client_destroy;
	struct wl_listener display_destroy;
};

struct nx_xwayland_server *nx_xwayland_server_create(struct wl_display *display);
void nx_xwayland_server_destroy(struct nx_xwayland_server *server);

void xwayland_server_spawn(struct nx_xwayland_server *server);
void xwayland_server_finish_process(struct nx_xwayland_server *server);
void xwayland_server_finish_display(struct nx_xwayland_server *server);

#endif
