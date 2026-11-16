#include "layer.h"

#include <poll.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#define FIRST_WAIT_MS 200           /* device, swapchain creation */

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic(struct vrs_link *) page;
static int held = -1;               /* socket behind page */
static int pending = -1;            /* dialled, page not received */

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

	if (recvmsg(sock, &msg, MSG_CMSG_CLOEXEC | MSG_DONTWAIT) != 1)
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
	struct vrs_link_address a = vrs_link_address();
	int sock = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);

	if (sock < 0)
		return -1;
	if (connect(sock, (struct sockaddr *)&a.addr, a.len) == 0)
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

static int
readable(int sock, int timeout_ms)
{
	struct pollfd p = { .fd = sock, .events = POLLIN };

	return poll(&p, 1, timeout_ms) > 0;
}

/* Lock held; bounded by timeout. */
static void
collect(int timeout_ms)
{
	struct vrs_link *map;
	int sock;

	if (pending < 0)
		pending = dial();
	if (pending < 0 || !readable(pending, timeout_ms))
		return;
	sock = pending;
	pending = -1;
	map = map_page(sock);
	if (!map) {
		close(sock);
		return;
	}
	atomic_store(&page, map);
	held = sock;
}

/* Held socket; EOF frees peer. */
void
link_connect(void)
{
	pthread_mutex_lock(&lock);
	if (!atomic_load(&page))
		collect(FIRST_WAIT_MS);
	pthread_mutex_unlock(&lock);
}

/* Never unmaps: readers may hold. */
void
link_check(void)
{
	char byte;

	pthread_mutex_lock(&lock);
	if (held >= 0 && recv(held, &byte, 1, MSG_PEEK | MSG_DONTWAIT) == 0) {
		close(held);
		held = -1;
		atomic_store(&page, NULL);
	}
	if (!atomic_load(&page))
		collect(0);
	pthread_mutex_unlock(&lock);
}

struct vrs_link *
link_page(void)
{
	return atomic_load(&page);
}
