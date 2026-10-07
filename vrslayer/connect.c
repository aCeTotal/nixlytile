#include "layer.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#define RECEIVE_TIMEOUT_US 200000

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic(struct vrs_link *) page;

static int
receive_fd(int sock)
{
	char byte;
	struct iovec iov = { .iov_base = &byte, .iov_len = 1 };
	union {
		char buf[CMSG_SPACE(sizeof(int))];
		struct cmsghdr align;
	} ctl;
	struct msghdr msg = {
		.msg_iov = &iov,
		.msg_iovlen = 1,
		.msg_control = ctl.buf,
		.msg_controllen = sizeof(ctl.buf),
	};
	struct cmsghdr *c;
	int fd;

	if (recvmsg(sock, &msg, MSG_CMSG_CLOEXEC) != 1)
		return -1;
	c = CMSG_FIRSTHDR(&msg);
	if (!c || c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS)
		return -1;
	memcpy(&fd, CMSG_DATA(c), sizeof(fd));
	return fd;
}

static int
dial(void)
{
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	struct timeval timeout = { .tv_usec = RECEIVE_TIMEOUT_US };
	socklen_t len;
	int sock, n;

	n = snprintf(addr.sun_path + 1, sizeof(addr.sun_path) - 1,
		VRS_LINK_SOCKET, (unsigned int)getuid());
	len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + (size_t)n);
	sock = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (sock < 0)
		return -1;
	if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0
			&& connect(sock, (struct sockaddr *)&addr, len) == 0)
		return sock;
	close(sock);
	return -1;
}

static struct vrs_link *
map_page(int sock)
{
	struct vrs_link *map;
	int fd = receive_fd(sock);

	if (fd < 0)
		return NULL;
	map = mmap(NULL, sizeof(*map), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	close(fd);
	if (map == MAP_FAILED)
		return NULL;
	if (map->magic == VRS_LINK_MAGIC && map->version == VRS_LINK_VERSION)
		return map;
	munmap(map, sizeof(*map));
	return NULL;
}

/* Held socket; EOF frees peer. */
void
link_connect(void)
{
	struct vrs_link *map;
	int sock;

	pthread_mutex_lock(&lock);
	sock = atomic_load(&page) ? -1 : dial();
	map = sock < 0 ? NULL : map_page(sock);
	if (map)
		atomic_store(&page, map);
	else if (sock >= 0)
		close(sock);
	pthread_mutex_unlock(&lock);
}

struct vrs_link *
link_page(void)
{
	return atomic_load(&page);
}
