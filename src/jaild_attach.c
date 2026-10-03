#include "jaild.h"

#include <fcntl.h>
#include <limits.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define BLACKLIST_DIR "/run/firejail/firejail.ro.dir"
#define DIR_FLAGS     (O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)

static int
same_node(int fd, const struct stat *ref)
{
	struct stat st;

	return fstat(fd, &st) == 0 && st.st_dev == ref->st_dev &&
		st.st_ino == ref->st_ino;
}

/* Empty tmpfs over a blacklisted dir. */
static int
overmount(int dir)
{
	int fs = fsopen("tmpfs", FSOPEN_CLOEXEC), mnt = -1, r = -1;

	if (fs < 0)
		return -1;
	if (fsconfig(fs, FSCONFIG_SET_STRING, "mode", "0755", 0) == 0 &&
			fsconfig(fs, FSCONFIG_CMD_CREATE, NULL, NULL, 0) == 0)
		mnt = fsmount(fs, FSMOUNT_CLOEXEC, MOUNT_ATTR_NOSUID |
			MOUNT_ATTR_NODEV | MOUNT_ATTR_NOEXEC);
	if (mnt >= 0)
		r = move_mount(mnt, "", dir, "",
			MOVE_MOUNT_F_EMPTY_PATH | MOVE_MOUNT_T_EMPTY_PATH);
	if (mnt >= 0)
		close(mnt);
	close(fs);
	return r;
}

static int
enter(int dir, const char *comp, const struct ucred *peer,
	const struct stat *blacklist)
{
	int next, r;

	if (mkdirat(dir, comp, 0700) == 0)
		(void)!fchownat(dir, comp, peer->uid, peer->gid,
			AT_SYMLINK_NOFOLLOW);
	next = openat(dir, comp, DIR_FLAGS);
	if (next < 0 || !same_node(next, blacklist))
		return next;
	r = overmount(next);
	close(next);
	return r < 0 ? -1 : openat(dir, comp, DIR_FLAGS);
}

/* Symlink-free jail-side parent lookup. */
static int
open_parent(char *path, const struct ucred *peer,
	const struct stat *blacklist, char **name)
{
	int dir = open("/", DIR_FLAGS);
	char *comp = path + 1, *slash;

	while (dir >= 0 && (slash = strchr(comp, '/'))) {
		int next;

		*slash = '\0';
		next = enter(dir, comp, peer, blacklist);
		close(dir);
		dir = next;
		comp = slash + 1;
	}
	*name = comp;
	return dir;
}

static int
attach(const Pick *p, const struct ucred *peer, const struct stat *blacklist)
{
	char path[PATH_MAX], *name;
	int parent, spot, r;

	snprintf(path, sizeof(path), "%s", p->path);
	if ((parent = open_parent(path, peer, blacklist, &name)) < 0)
		return -1;
	if (p->dir)
		mkdirat(parent, name, 0700);
	spot = p->dir
		? openat(parent, name, DIR_FLAGS)
		: openat(parent, name, O_RDONLY | O_CREAT | O_NOFOLLOW |
			O_NONBLOCK | O_CLOEXEC, 0600);
	close(parent);
	if (spot < 0)
		return -1;
	r = move_mount(p->tree, "", spot, "",
		MOVE_MOUNT_F_EMPTY_PATH | MOVE_MOUNT_T_EMPTY_PATH);
	close(spot);
	return r;
}

static int
attach_all(const Grant *g, int ns)
{
	struct stat blacklist = { 0 };

	if (setns(ns, CLONE_NEWNS) < 0)
		return 1;
	stat(BLACKLIST_DIR, &blacklist);
	for (int i = 0; i < g->n; i++)
		if (attach(&g->picks[i], &g->peer, &blacklist) < 0)
			return 1;
	return 0;
}

const char *
jail_inject(const Grant *g)
{
	char path[32];
	int ns, status;
	pid_t child;

	snprintf(path, sizeof(path), "/proc/%d/ns/mnt", (int)g->target);
	if ((ns = open(path, O_RDONLY | O_CLOEXEC)) < 0)
		return "ns";
	child = fork();
	if (child == 0)
		_exit(attach_all(g, ns));
	close(ns);
	if (child < 0 || waitpid(child, &status, 0) < 0 ||
			!WIFEXITED(status) || WEXITSTATUS(status) != 0)
		return "mount";
	return NULL;
}
