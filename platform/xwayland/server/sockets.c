#undef _POSIX_C_SOURCE
#define _XOPEN_SOURCE 700 // for S_ISVTX
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#include "platform/log/log.h"
#include "platform/os/fd.h"
#include "platform/xwayland/server/sockets.h"

#define MAX_DISPLAY 32
#define LOCK_SIZE 11 // "%10d" and its terminator

static const char lock_fmt[] = "/tmp/.X%d-lock";
static const char socket_dir[] = "/tmp/.X11-unix";
static const char socket_fmt[] = "/tmp/.X11-unix/X%d";

enum lock_state {
	LOCK_OWNED,
	LOCK_HELD,
	LOCK_FAILED,
};

// '@' marks an abstract socket.
static char path_prefix(const struct sockaddr_un *addr) {
	return addr->sun_path[0] ? addr->sun_path[0] : '@';
}

static int close_socket(int fd, const struct sockaddr_un *addr) {
	close(fd);
	if (addr->sun_path[0]) {
		unlink(addr->sun_path);
	}
	return -1;
}

static int open_socket(struct sockaddr_un *addr, size_t path_size) {
	socklen_t size = offsetof(struct sockaddr_un, sun_path) + path_size + 1;

	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		nx_log_errno(NX_ERROR, "Failed to create socket %c%s",
			path_prefix(addr), addr->sun_path + 1);
		return -1;
	}
	if (!set_cloexec(fd, true)) {
		close(fd);
		return -1;
	}

	if (addr->sun_path[0]) {
		unlink(addr->sun_path);
	}
	if (bind(fd, (struct sockaddr *)addr, size) < 0) {
		nx_log_errno(NX_ERROR, "Failed to bind socket %c%s",
			path_prefix(addr), addr->sun_path + 1);
		return close_socket(fd, addr);
	}
	if (listen(fd, 1) < 0) {
		nx_log_errno(NX_ERROR, "Failed to listen to socket %c%s",
			path_prefix(addr), addr->sun_path + 1);
		return close_socket(fd, addr);
	}
	return fd;
}

static bool check_socket_dir(void) {
	struct stat buf;

	if (lstat(socket_dir, &buf)) {
		nx_log_errno(NX_ERROR, "Failed to stat %s", socket_dir);
		return false;
	}
	if (!(buf.st_mode & S_IFDIR)) {
		nx_log(NX_ERROR, "%s is not a directory", socket_dir);
		return false;
	}
	if (buf.st_uid != 0 && buf.st_uid != getuid()) {
		nx_log(NX_ERROR, "%s not owned by root or us", socket_dir);
		return false;
	}
	// Unsticky dirs must be private.
	if (!(buf.st_mode & S_ISVTX) && (buf.st_mode & (S_IWGRP | S_IWOTH))) {
		nx_log(NX_ERROR, "sticky bit not set on %s", socket_dir);
		return false;
	}
	return true;
}

static bool prepare_socket_dir(void) {
	if (mkdir(socket_dir, 0755) == 0) {
		nx_log(NX_INFO, "Created %s ourselves -- other users will "
			"be unable to create X11 UNIX sockets of their own",
			socket_dir);
		return true;
	}
	if (errno != EEXIST) {
		nx_log_errno(NX_ERROR, "Unable to mkdir %s", socket_dir);
		return false;
	}
	return check_socket_dir();
}

// Abstract socket first, then path.
static bool open_sockets(int socks[2], int display) {
	if (!prepare_socket_dir()) {
		return false;
	}

	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	size_t path_size = snprintf(addr.sun_path + 1, sizeof(addr.sun_path) - 1,
		socket_fmt, display);
	socks[0] = open_socket(&addr, path_size);
	if (socks[0] < 0) {
		return false;
	}

	path_size = snprintf(addr.sun_path, sizeof(addr.sun_path), socket_fmt, display);
	socks[1] = open_socket(&addr, path_size);
	if (socks[1] < 0) {
		close(socks[0]);
		socks[0] = -1;
		return false;
	}
	return true;
}

void xwayland_unlink_display_sockets(int display) {
	char sun_path[64];

	snprintf(sun_path, sizeof(sun_path), socket_fmt, display);
	unlink(sun_path);

	snprintf(sun_path, sizeof(sun_path), lock_fmt, display);
	unlink(sun_path);
}

static enum lock_state create_lock(int socks[2], int display,
		const char *lock_name) {
	int fd = open(lock_name, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0444);
	if (fd < 0) {
		return LOCK_HELD;
	}

	char pid[LOCK_SIZE + 1];
	snprintf(pid, sizeof(pid), "%10d", getpid());
	if (!open_sockets(socks, display) || write(fd, pid, LOCK_SIZE) != LOCK_SIZE) {
		unlink(lock_name);
		close(fd);
		return LOCK_FAILED;
	}
	close(fd);
	return LOCK_OWNED;
}

static bool lock_is_stale(const char *lock_name) {
	int fd = open(lock_name, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		return false;
	}

	char pid[LOCK_SIZE + 1] = { 0 };
	ssize_t bytes = read(fd, pid, LOCK_SIZE);
	close(fd);
	if (bytes != LOCK_SIZE) {
		return false;
	}

	char *end;
	long owner = strtol(pid, &end, 10);
	if (owner < 0 || owner > INT32_MAX || end != pid + LOCK_SIZE - 1) {
		return false;
	}
	errno = 0;
	return kill((pid_t)owner, 0) != 0 && errno == ESRCH;
}

int xwayland_open_display_sockets(int socks[2]) {
	char lock_name[64];

	for (int display = 0; display <= MAX_DISPLAY; display++) {
		snprintf(lock_name, sizeof(lock_name), lock_fmt, display);
		enum lock_state state = create_lock(socks, display, lock_name);
		if (state == LOCK_OWNED) {
			return display;
		}
		// Retry after a dead owner.
		if (state == LOCK_HELD && lock_is_stale(lock_name) && unlink(lock_name) == 0) {
			display--;
		}
	}

	nx_log(NX_ERROR, "No display available in the first %d", MAX_DISPLAY + 1);
	return -1;
}
