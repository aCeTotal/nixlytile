#include "nixlytile.h"
#include "vrslayer/link.h"

#include <sys/mman.h>

#define VRSLINK_PEERS   32
#define VRSLINK_BACKLOG 8

struct vrs_peer {
	int fd;
	pid_t pid;
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
	dynrender_link_closed(p->page);
	wl_event_source_remove(p->source);
	munmap(p->page, sizeof(*p->page));
	close(p->fd);
	p->fd = -1;
	vrslink_generation++;
}

static int
peer_readable(int fd, uint32_t mask, void *data)
{
	char sink[64];
	ssize_t n;

	while ((n = read(fd, sink, sizeof(sink))) > 0)
		;
	if (n < 0 && errno == EAGAIN)
		return 0;
	peer_close(data);
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
page_create(int *memfd)
{
	struct vrs_link *page;

	*memfd = memfd_create("nixly-vrs", MFD_CLOEXEC | MFD_ALLOW_SEALING);
	if (*memfd < 0)
		return NULL;
	if (ftruncate(*memfd, sizeof(*page)) < 0 || fcntl(*memfd, F_ADD_SEALS,
			F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL) < 0)
		return NULL;
	page = mmap(NULL, sizeof(*page), PROT_READ | PROT_WRITE, MAP_SHARED,
		*memfd, 0);
	if (page == MAP_FAILED)
		return NULL;
	page->magic = VRS_LINK_MAGIC;
	page->version = VRS_LINK_VERSION;
	atomic_store(&page->cursor, VRS_CURSOR_NONE);
	return page;
}

static void
peer_open(struct vrs_peer *p, int conn, pid_t pid)
{
	int memfd = -1;
	struct vrs_link *page = page_create(&memfd);
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
	p->page = page;
	p->source = source;
	vrslink_generation++;
}

static struct vrs_peer *
free_peer(void)
{
	int i;

	for (i = 0; i < VRSLINK_PEERS; i++)
		if (peers[i].fd < 0)
			return &peers[i];
	return NULL;
}

static int
listen_readable(int fd, uint32_t mask, void *data)
{
	struct ucred cred;
	socklen_t len = sizeof(cred);
	struct vrs_peer *p;
	int conn;

	while ((conn = accept4(fd, NULL, NULL,
			SOCK_NONBLOCK | SOCK_CLOEXEC)) >= 0) {
		p = free_peer();
		if (!p || getsockopt(conn, SOL_SOCKET, SO_PEERCRED, &cred,
				&len) < 0 || cred.uid != getuid()) {
			close(conn);
			continue;
		}
		peer_open(p, conn, cred.pid);
	}
	return 0;
}

void
vrslink_target(struct vrs_link *page, uint64_t interval_ns, uint64_t budget_ns)
{
	atomic_store(&page->interval_ns, (uint32_t)interval_ns);
	atomic_store(&page->budget_ns, (uint32_t)budget_ns);
	atomic_store(&page->active, interval_ns > 0);
}

void
vrslink_idle(struct vrs_link *page)
{
	atomic_store(&page->active, 0);
}

static uint32_t
pack_axis(double pos)
{
	return (uint32_t)(MIN(MAX(pos, 0.0), 1.0) * VRS_CURSOR_SCALE);
}

/* Hidden pointer: center focus only. */
static uint32_t
cursor_focus(Client *game)
{
	struct wlr_surface *s = client_surface(game);
	double sx, sy;

	if (cursor_hidden_by_client || !s || !game->scene_surface
			|| seat->pointer_state.focused_surface != s
			|| s->current.width <= 0 || s->current.height <= 0)
		return VRS_CURSOR_NONE;
	if (active_constraint
			&& active_constraint->type == WLR_POINTER_CONSTRAINT_V1_LOCKED)
		return VRS_CURSOR_NONE;
	sx = (cursor->x - game->geom.x - game->scene_surface->node.x) / s->current.width;
	sy = (cursor->y - game->geom.y - game->scene_surface->node.y) / s->current.height;
	return pack_axis(sx) << 16 | pack_axis(sy);
}

void
vrslink_cursor(struct vrs_link *page, Client *game)
{
	atomic_store(&page->cursor, cursor_focus(game));
}

struct vrs_link *
vrslink_for_pid(pid_t pid)
{
	int i;

	for (i = 0; i < VRSLINK_PEERS; i++)
		if (peers[i].fd >= 0 && peers[i].pid == pid)
			return peers[i].page;
	return NULL;
}

/* Abstract socket: reachable inside pressure-vessel. */
void
vrslink_setup(void)
{
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	int i, n;

	for (i = 0; i < VRSLINK_PEERS; i++)
		peers[i].fd = -1;
	n = snprintf(addr.sun_path + 1, sizeof(addr.sun_path) - 1,
		VRS_LINK_SOCKET, (unsigned int)getuid());
	listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (listen_fd < 0)
		return;
	if (bind(listen_fd, (struct sockaddr *)&addr,
			(socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + n)) < 0
			|| listen(listen_fd, VRSLINK_BACKLOG) < 0) {
		wlr_log(WLR_ERROR, "vrslink: %s", strerror(errno));
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
		if (peers[i].fd >= 0)
			peer_close(&peers[i]);
	if (listen_source)
		wl_event_source_remove(listen_source);
	if (listen_fd >= 0)
		close(listen_fd);
	listen_source = NULL;
	listen_fd = -1;
}
