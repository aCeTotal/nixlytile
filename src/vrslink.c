#include "nixlytile.h"
#include "vrslayer/link.h"

#include <sys/mman.h>

#define VRSLINK_PEERS   32
#define VRSLINK_BACKLOG 8
#define VRSLINK_DRAIN   64
#define VRSLINK_MEMFD   "nixly-vrs"

struct vrs_peer {
	int fd;
	pid_t pid;
	unsigned int opened;        /* newest wins on pid reuse */
	struct vrs_link *page;
	struct wl_event_source *source;
};

static struct vrs_peer peers[VRSLINK_PEERS];
static int listen_fd = -1;
static struct wl_event_source *listen_source;
unsigned int vrslink_generation;

static void
peer_close(struct vrs_peer *p)
{
	if (p->fd < 0)
		return;
	foveation_page_closed(p->page);
	wl_event_source_remove(p->source);
	munmap(p->page, sizeof(*p->page));
	close(p->fd);
	p->fd = -1;
	vrslink_generation++;
}

static int
peer_readable(int fd, uint32_t mask, void *peer)
{
	char sink[VRSLINK_DRAIN];
	ssize_t n;

	while ((n = read(fd, sink, sizeof(sink))) > 0)
		;
	if (n < 0 && errno == EAGAIN)
		return 0;
	peer_close(peer);
	return 0;
}

static int
send_fd(int sock, int fd)
{
	char byte = 0;
	struct iovec iov = { .iov_base = &byte, .iov_len = 1 };
	union {
		char buf[CMSG_SPACE(sizeof(int))];
		struct cmsghdr align;
	} ctl = {0};
	struct msghdr msg = {
		.msg_iov = &iov,
		.msg_iovlen = 1,
		.msg_control = ctl.buf,
		.msg_controllen = sizeof(ctl.buf),
	};
	struct cmsghdr *c = CMSG_FIRSTHDR(&msg);

	c->cmsg_level = SOL_SOCKET;
	c->cmsg_type = SCM_RIGHTS;
	c->cmsg_len = CMSG_LEN(sizeof(int));
	memcpy(CMSG_DATA(c), &fd, sizeof(int));
	return sendmsg(sock, &msg, MSG_NOSIGNAL) == 1;
}

/* Sealed against peer shrinking. */
static struct vrs_link *
page_create(int memfd)
{
	struct vrs_link *page;

	if (ftruncate(memfd, sizeof(*page)) < 0 || fcntl(memfd, F_ADD_SEALS,
			F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL) < 0)
		return NULL;
	page = mmap(NULL, sizeof(*page), PROT_READ | PROT_WRITE, MAP_SHARED,
		memfd, 0);
	if (page == MAP_FAILED)
		return NULL;
	page->magic = VRS_LINK_MAGIC;
	page->version = VRS_LINK_VERSION;
	return page;
}

static void
peer_open(struct vrs_peer *p, int conn, pid_t pid)
{
	int memfd = memfd_create(VRSLINK_MEMFD, MFD_CLOEXEC | MFD_ALLOW_SEALING);
	struct vrs_link *page = memfd >= 0 ? page_create(memfd) : NULL;
	struct wl_event_source *source = page && send_fd(conn, memfd)
		? wl_event_loop_add_fd(event_loop, conn, WL_EVENT_READABLE, peer_readable, p)
		: NULL;

	if (memfd >= 0)
		close(memfd);
	if (page && !source)
		munmap(page, sizeof(*page));
	if (!source) {
		close(conn);
		return;
	}
	p->fd = conn;
	p->pid = pid;
	p->opened = ++vrslink_generation;
	p->page = page;
	p->source = source;
}

static struct vrs_peer *
free_peer(void)
{
	int i = 0;

	while (i < VRSLINK_PEERS && peers[i].fd >= 0)
		i++;
	return i < VRSLINK_PEERS ? &peers[i] : NULL;
}

/* Same user only. */
static void
peer_accept(int conn)
{
	struct ucred cred;
	socklen_t len = sizeof(cred);
	struct vrs_peer *p = free_peer();

	if (!p || getsockopt(conn, SOL_SOCKET, SO_PEERCRED, &cred, &len) < 0
			|| cred.uid != getuid()) {
		close(conn);
		return;
	}
	peer_open(p, conn, cred.pid);
}

static int
listen_readable(int fd, uint32_t mask, void *unused)
{
	int conn;

	while ((conn = accept4(fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC)) >= 0)
		peer_accept(conn);
	return 0;
}

/* Unchanged: cache line stays shared. */
static void
store_changed(_Atomic uint32_t *field, uint32_t value)
{
	if (atomic_load_explicit(field, memory_order_relaxed) != value)
		atomic_store(field, value);
}

/* Zones land before active. */
void
vrslink_publish(struct vrs_link *page, uint32_t focus)
{
	store_changed(&page->focus, focus);
	store_changed(&page->fovea, vrs_zones.fovea);
	store_changed(&page->blend, vrs_zones.blend);
	store_changed(&page->quality, vrs_zones.quality);
	store_changed(&page->active, 1);
}

/* Inactive pages render native. */
void
vrslink_idle(struct vrs_link *page)
{
	store_changed(&page->active, 0);
}

/* Live pid peer, opened later. */
static int
newer(const struct vrs_peer *p, const struct vrs_peer *best, pid_t pid)
{
	return p->fd >= 0 && p->pid == pid
		&& (!best || (int)(p->opened - best->opened) > 0);
}

struct vrs_link *
vrslink_for_pid(pid_t pid)
{
	struct vrs_peer *best = NULL;
	int i;

	for (i = 0; i < VRSLINK_PEERS; i++)
		best = newer(&peers[i], best, pid) ? &peers[i] : best;
	return best ? best->page : NULL;
}

/* Abstract socket: reachable inside pressure-vessel. */
void
vrslink_setup(void)
{
	struct vrs_link_address a = vrs_link_address();
	int i;

	for (i = 0; i < VRSLINK_PEERS; i++)
		peers[i].fd = -1;
	listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (listen_fd < 0)
		return;
	if (bind(listen_fd, (struct sockaddr *)&a.addr, a.len) < 0
			|| listen(listen_fd, VRSLINK_BACKLOG) < 0) {
		nx_log(NX_ERROR, "vrslink: %s", strerror(errno));
		close(listen_fd);
		listen_fd = -1;
		return;
	}
	listen_source = wl_event_loop_add_fd(event_loop, listen_fd,
		WL_EVENT_READABLE, listen_readable, NULL);
}

void
vrslink_cleanup(void)
{
	int i;

	for (i = 0; i < VRSLINK_PEERS; i++)
		peer_close(&peers[i]);
	if (listen_source)
		wl_event_source_remove(listen_source);
	if (listen_fd >= 0)
		close(listen_fd);
	listen_source = NULL;
	listen_fd = -1;
}
