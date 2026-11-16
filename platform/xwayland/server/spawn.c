#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdnoreturn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include "platform/log/log.h"
#include "platform/os/fd.h"
#include "platform/xwayland/server/server.h"

static const char *xwayland_binary(void) {
	const char *path = getenv("NIXLY_XWAYLAND");
	if (path == NULL) {
		return XWAYLAND_PATH;
	}
	nx_log(NX_INFO, "Using Xwayland binary '%s' due to NIXLY_XWAYLAND", path);
	return path;
}

// Mutes output below log level.
static int mute_output(void) {
	enum nx_log_importance verbosity = nx_log_get_verbosity();
	int devnull = open("/dev/null", O_WRONLY | O_CREAT | O_CLOEXEC, 0666);
	if (devnull < 0) {
		nx_log_errno(NX_ERROR, "XWayland: failed to open /dev/null");
		_exit(EXIT_FAILURE);
	}
	if (verbosity < NX_INFO) {
		dup2(devnull, STDOUT_FILENO);
	}
	if (verbosity < NX_ERROR) {
		dup2(devnull, STDERR_FILENO);
	}
	return devnull;
}

noreturn static void exec_xwayland(struct nx_xwayland_server *server,
		int notify_fd) {
	if (!set_cloexec(server->x_fd[0], false) ||
			!set_cloexec(server->x_fd[1], false) ||
			!set_cloexec(server->wl_fd[1], false) ||
			!set_cloexec(server->wm_fd[1], false)) {
		nx_log(NX_ERROR, "Failed to unset CLOEXEC on FD");
		_exit(EXIT_FAILURE);
	}

	char listenfd0[16], listenfd1[16], displayfd[16], wmfd[16];
	snprintf(listenfd0, sizeof(listenfd0), "%d", server->x_fd[0]);
	snprintf(listenfd1, sizeof(listenfd1), "%d", server->x_fd[1]);
	snprintf(displayfd, sizeof(displayfd), "%d", notify_fd);
	snprintf(wmfd, sizeof(wmfd), "%d", server->wm_fd[1]);

	char *argv[] = {
		"Xwayland", server->display_name, "-rootless", "-core",
		"-listenfd", listenfd0, "-listenfd", listenfd1,
		"-displayfd", displayfd,
		"-wm", wmfd,
		NULL,
	};

	char wayland_socket[16];
	snprintf(wayland_socket, sizeof(wayland_socket), "%d", server->wl_fd[1]);
	setenv("WAYLAND_SOCKET", wayland_socket, true);

	nx_log(NX_INFO, "Starting Xwayland on :%d", server->display);

	int devnull = mute_output();
	const char *path = xwayland_binary();
	execvp(path, argv);

	nx_log_errno(NX_ERROR, "failed to exec %s", path);
	close(devnull);
	_exit(EXIT_FAILURE);
}

// Double fork: init reaps Xwayland.
noreturn static void run_child(struct nx_xwayland_server *server, int notify_fd) {
	pid_t pid = fork();
	if (pid < 0) {
		nx_log_errno(NX_ERROR, "second fork failed");
		_exit(EXIT_FAILURE);
	}
	if (pid == 0) {
		exec_xwayland(server, notify_fd);
	}
	_exit(EXIT_SUCCESS);
}

// True when done reading or failed.
static bool read_ready_line(int fd, uint32_t *mask) {
	// Xwayland writes twice; read both.
	char buf[64];
	ssize_t n = read(fd, buf, sizeof(buf));
	if (n < 0 && errno != EINTR) {
		nx_log_errno(NX_ERROR, "read from Xwayland display_fd failed");
		*mask = 0;
		return true;
	}
	return n > 0 && buf[n - 1] == '\n';
}

static bool reap_fork(pid_t pid) {
	while (waitpid(pid, NULL, 0) < 0) {
		if (errno == EINTR) {
			continue;
		}
		// Another SIGCHLD handler reaped it.
		if (errno == ECHILD) {
			return true;
		}
		nx_log_errno(NX_ERROR, "waitpid for Xwayland fork failed");
		return false;
	}
	return true;
}

static int abort_start(struct nx_xwayland_server *server, int fd) {
	close(fd);
	xwayland_server_finish_process(server);
	xwayland_server_finish_display(server);
	return 0;
}

static int handle_ready(int fd, uint32_t mask, void *data) {
	struct nx_xwayland_server *server = data;

	if ((mask & WL_EVENT_READABLE) && !read_ready_line(fd, &mask)) {
		return 1;
	}
	if (!reap_fork(server->pid)) {
		return abort_start(server, fd);
	}
	// Xwayland writes only after setup.
	if (!(mask & WL_EVENT_READABLE)) {
		assert(mask & WL_EVENT_HANGUP);
		nx_log(NX_ERROR, "Xwayland startup failed, not setting up xwm");
		return abort_start(server, fd);
	}
	nx_log(NX_DEBUG, "Xserver is ready");

	close(fd);
	wl_event_source_remove(server->pipe_source);
	server->pipe_source = NULL;
	wl_signal_emit_mutable(&server->events.ready, NULL);
	return 0;
}

static bool open_socketpair(int fds[2]) {
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
		nx_log_errno(NX_ERROR, "socketpair failed");
		return false;
	}
	if (!set_cloexec(fds[0], true) || !set_cloexec(fds[1], true)) {
		nx_log(NX_ERROR, "Failed to set O_CLOEXEC on socket");
		return false;
	}
	return true;
}

static bool open_notify_pipe(int fds[2]) {
	if (pipe(fds) == -1) {
		nx_log_errno(NX_ERROR, "pipe failed");
		return false;
	}
	if (!set_cloexec(fds[0], true)) {
		nx_log(NX_ERROR, "Failed to set CLOEXEC on FD");
		close(fds[0]);
		close(fds[1]);
		return false;
	}
	return true;
}

void xwayland_server_spawn(struct nx_xwayland_server *server) {
	if (!open_socketpair(server->wl_fd) || !open_socketpair(server->wm_fd)) {
		xwayland_server_finish_process(server);
		return;
	}

	server->client = wl_client_create(server->wl_display, server->wl_fd[0]);
	if (!server->client) {
		nx_log_errno(NX_ERROR, "wl_client_create failed");
		xwayland_server_finish_process(server);
		return;
	}
	server->wl_fd[0] = -1; // owned by the client
	wl_client_add_destroy_listener(server->client, &server->client_destroy);

	int notify_fd[2];
	if (!open_notify_pipe(notify_fd)) {
		xwayland_server_finish_process(server);
		return;
	}

	struct wl_event_loop *loop = wl_display_get_event_loop(server->wl_display);
	server->pipe_source = wl_event_loop_add_fd(loop, notify_fd[0],
		WL_EVENT_READABLE, handle_ready, server);

	wl_signal_emit_mutable(&server->events.start, NULL);

	server->pid = fork();
	if (server->pid < 0) {
		nx_log_errno(NX_ERROR, "fork failed");
		close(notify_fd[0]);
		close(notify_fd[1]);
		xwayland_server_finish_process(server);
		return;
	}
	if (server->pid == 0) {
		run_child(server, notify_fd[1]);
	}

	// X sockets stay for restarts.
	close(notify_fd[1]);
	close(server->wl_fd[1]);
	close(server->wm_fd[1]);
	server->wl_fd[1] = server->wm_fd[1] = -1;
}
