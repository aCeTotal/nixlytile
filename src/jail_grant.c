#include "nixlytile.h"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

#define JAILD_SOCK "/run/nixly-jaild.sock"

static int
same_inode(const char *a, const char *b)
{
	struct stat sa, sb;

	if (stat(a, &sa) != 0 || stat(b, &sb) != 0)
		return 0;
	return sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
}

int
jail_hides(pid_t pid, const char *path)
{
	char ns[64], inside[PATH_MAX + 32];

	snprintf(ns, sizeof(ns), "/proc/%d/ns/mnt", (int)pid);
	if (same_inode(ns, "/proc/self/ns/mnt"))
		return 0;
	snprintf(inside, sizeof(inside), "/proc/%d/root%s", (int)pid, path);
	return !same_inode(inside, path);
}

int
jail_grant_send(const char *req, size_t len)
{
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);

	if (fd < 0)
		return -1;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", JAILD_SOCK);
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
			write(fd, req, len) != (ssize_t)len) {
		close(fd);
		return -1;
	}
	shutdown(fd, SHUT_WR);
	return fd;
}

int
jail_grant_ok(int fd)
{
	char ans[64];
	ssize_t n = read(fd, ans, sizeof(ans) - 1);

	if (n <= 0)
		return 0;
	ans[n] = '\0';
	if (strncmp(ans, "ok", 2) == 0)
		return 1;
	wlr_log(WLR_INFO, "jail_grant: %s", ans);
	return 0;
}
