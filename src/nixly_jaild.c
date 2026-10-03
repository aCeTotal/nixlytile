#include "jaild.h"

#include <ctype.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/fsuid.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "priv_sock.h"

#define SOCK_PATH    "/run/nixly-jaild.sock"
#define ACTION_ID    "org.nixlytile.jail-grant"
#define REQ_MAX      65536
#define FILES_MAX    4096
#define APP_MAX      96
#define ENTITY_MAX   6
#define GROUPS_MAX   256
#define IO_TIMEOUT_S 2

typedef struct {
	char app[APP_MAX];
	char files[FILES_MAX];
} Prompt;

static unsigned long long
start_time(pid_t pid)
{
	char path[32], line[1024], *p;
	unsigned long long start = 0;
	FILE *fp;

	snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
	if (!(fp = fopen(path, "r")))
		return 0;
	if (fgets(line, sizeof(line), fp) && (p = strrchr(line, ')')))
		sscanf(p + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u "
			"%*u %*u %*d %*d %*d %*d %*d %*d %llu", &start);
	fclose(fp);
	return start;
}

static void
comm_of(pid_t pid, char *out, size_t len)
{
	char path[32];
	FILE *fp;

	snprintf(out, len, "?");
	snprintf(path, sizeof(path), "/proc/%d/comm", (int)pid);
	if (!(fp = fopen(path, "r")))
		return;
	if (fgets(out, (int)len, fp))
		out[strcspn(out, "\n")] = '\0';
	fclose(fp);
}

static const char *
check_target(const Grant *g)
{
	char path[32];
	struct stat proc, ns, host;

	snprintf(path, sizeof(path), "/proc/%d", (int)g->target);
	if (g->target <= 1 || stat(path, &proc) != 0 || proc.st_uid != g->peer.uid)
		return "target";
	snprintf(path, sizeof(path), "/proc/%d/ns/mnt", (int)g->target);
	if (stat(path, &ns) != 0 || stat("/proc/1/ns/mnt", &host) != 0)
		return "target";
	return ns.st_ino == host.st_ino ? "not-jailed" : NULL;
}

/* Filesystem access as the peer. */
static int
become_peer(const struct ucred *peer)
{
	struct passwd *pw = getpwuid(peer->uid);
	gid_t groups[GROUPS_MAX];
	int n = GROUPS_MAX;

	if (!pw || getgrouplist(pw->pw_name, peer->gid, groups, &n) < 0 ||
			setgroups((size_t)n, groups) < 0)
		return -1;
	setfsgid(peer->gid);
	setfsuid(peer->uid);
	return (uid_t)setfsuid((uid_t)-1) == peer->uid ? 0 : -1;
}

static void
become_root(void)
{
	setfsuid(0);
	setfsgid(0);
	setgroups(0, NULL);
}

static const char *
open_pick(Pick *p)
{
	struct mount_attr attr = { .attr_set = MOUNT_ATTR_RDONLY |
		MOUNT_ATTR_NOSUID | MOUNT_ATTR_NODEV | MOUNT_ATTR_NOEXEC };
	char real[PATH_MAX];
	struct stat st;

	if (!realpath(p->path, real) || strcmp(real, p->path) != 0)
		return "path";
	if (lstat(p->path, &st) != 0 ||
			!(S_ISREG(st.st_mode) || S_ISDIR(st.st_mode)))
		return "type";
	p->dir = S_ISDIR(st.st_mode);
	if (faccessat(AT_FDCWD, p->path, p->dir ? R_OK | X_OK : R_OK,
			AT_EACCESS) != 0)
		return "access";
	p->tree = open_tree(AT_FDCWD, p->path, OPEN_TREE_CLONE |
		OPEN_TREE_CLOEXEC | AT_SYMLINK_NOFOLLOW | AT_RECURSIVE);
	if (p->tree < 0 || mount_setattr(p->tree, "", AT_EMPTY_PATH |
			AT_RECURSIVE, &attr, sizeof(attr)) < 0)
		return "clone";
	return NULL;
}

static const char *
open_picks(Grant *g)
{
	const char *err = NULL;

	if (become_peer(&g->peer) < 0)
		err = "creds";
	for (int i = 0; !err && i < g->n; i++)
		err = open_pick(&g->picks[i]);
	become_root();
	return err;
}

static const char *
parse(Grant *g, char *buf, size_t len)
{
	char *p = buf, *end = buf + len;

	if (len == 0 || buf[len - 1] != '\0')
		return "parse";
	g->target = (pid_t)atoi(p);
	for (p += strlen(p) + 1; p < end && g->n < PICKS_MAX; p += strlen(p) + 1)
		g->picks[g->n++] = (Pick){ .path = p, .tree = -1 };
	return g->n > 0 && p == end ? NULL : "parse";
}

static size_t
append_markup(char *out, size_t cap, size_t used, const char *s)
{
	static const char *const entity[UCHAR_MAX + 1] = {
		['&'] = "&amp;", ['<'] = "&lt;", ['>'] = "&gt;",
	};

	for (; *s && used + ENTITY_MAX < cap; s++) {
		const char *e = entity[(unsigned char)*s];

		if (!e) {
			out[used++] = *s;
			continue;
		}
		memcpy(out + used, e, strlen(e));
		used += strlen(e);
	}
	out[used] = '\0';
	return used;
}

static const char *
noun(const Grant *g)
{
	if (g->n > 1)
		return "filer";
	return g->picks[0].dir ? "mappe" : "fil";
}

static void
describe(const Grant *g, Prompt *pr)
{
	char comm[32];
	size_t used = 0;

	comm_of(g->target, comm, sizeof(comm));
	comm[0] = (char)toupper((unsigned char)comm[0]);
	append_markup(pr->app, sizeof(pr->app), 0, comm);
	for (int i = 0; i < g->n; i++) {
		const char *path = g->picks[i].path;
		const char *base = strrchr(path, '/') + 1;

		if (i)
			used = append_markup(pr->files, sizeof(pr->files), used, ", ");
		used = append_markup(pr->files, sizeof(pr->files), used,
			*base ? base : path);
	}
}

static int
authorize(const Grant *g)
{
	char subject[64];
	Prompt pr = { 0 };
	int status;
	pid_t child;

	snprintf(subject, sizeof(subject), "%d,%llu,%u", (int)g->peer.pid,
		start_time(g->peer.pid), (unsigned)g->peer.uid);
	describe(g, &pr);

	child = fork();
	if (child == 0) {
		execlp("pkcheck", "pkcheck", "--action-id", ACTION_ID,
			"--process", subject, "--allow-user-interaction",
			"--detail", "app", pr.app, "--detail", "what", noun(g),
			"--detail", "files", pr.files, (char *)NULL);
		_exit(127);
	}
	if (child < 0 || waitpid(child, &status, 0) < 0)
		return 0;
	return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static int
peer_gone(int fd)
{
	struct pollfd pfd = { .fd = fd, .events = POLLHUP };

	return poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLHUP);
}

static const char *
serve(int fd, Grant *g)
{
	static char buf[REQ_MAX];
	socklen_t clen = sizeof(g->peer);
	size_t len = 0;
	ssize_t n;
	const char *err;

	if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &g->peer, &clen) != 0)
		return "peer";
	while (len < sizeof(buf) &&
			(n = read(fd, buf + len, sizeof(buf) - len)) > 0)
		len += (size_t)n;
	if ((err = parse(g, buf, len)) || (err = check_target(g)) ||
			(err = open_picks(g)))
		return err;
	if (!authorize(g))
		return "denied";
	if (peer_gone(fd))
		return "cancelled";
	return jail_inject(g);
}

static void
handle(int fd)
{
	struct timeval tv = { .tv_sec = IO_TIMEOUT_S };
	Grant g = { 0 };
	const char *err;

	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	err = serve(fd, &g);
	for (int i = 0; i < g.n; i++)
		if (g.picks[i].tree >= 0)
			close(g.picks[i].tree);
	if (err)
		dprintf(fd, "err %s\n", err);
	else
		dprintf(fd, "ok\n");
}

int
main(void)
{
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	int lfd;

	signal(SIGPIPE, SIG_IGN);
	lfd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (lfd < 0) {
		perror("socket");
		return 1;
	}
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", SOCK_PATH);
	unlink(SOCK_PATH);
	if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
			listen(lfd, 8) < 0) {
		perror("bind");
		return 1;
	}
	sock_restrict(SOCK_PATH);

	for (;;) {
		int cfd = accept4(lfd, NULL, NULL, SOCK_CLOEXEC);

		if (cfd < 0)
			continue;
		if (peer_allowed(cfd))
			handle(cfd);
		close(cfd);
	}
}
