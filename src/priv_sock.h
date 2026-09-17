/*
 * Access control for the root helper sockets (nixly-diskd, nixly-fand):
 * root:wheel 0660 on the socket file, plus an SO_PEERCRED check so a
 * process that somehow holds the fd still has to be root or in wheel.
 * Header-only — both daemons build standalone from one .c file.
 */
#ifndef PRIV_SOCK_H
#define PRIV_SOCK_H

#include <grp.h>
#include <pwd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static inline gid_t
priv_wheel_gid(void)
{
	struct group *g = getgrnam("wheel");

	return g ? g->gr_gid : (gid_t)-1;
}

/* root:wheel 0660 — unprivileged service accounts cannot reach it. */
static inline void
sock_restrict(const char *path)
{
	gid_t wheel = priv_wheel_gid();

	if (wheel != (gid_t)-1)
		(void)!chown(path, 0, wheel);
	chmod(path, 0660);
}

static inline int
peer_allowed(int fd)
{
	struct ucred cr;
	socklen_t len = sizeof(cr);
	struct passwd *pw;
	gid_t wheel, groups[64];
	int ngroups = (int)(sizeof(groups) / sizeof(groups[0])), i;

	if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cr, &len) != 0)
		return 0;
	if (cr.uid == 0)
		return 1;
	wheel = priv_wheel_gid();
	if (wheel == (gid_t)-1)
		return 0;
	pw = getpwuid(cr.uid);
	if (!pw)
		return 0;
	if (pw->pw_gid == wheel)
		return 1;
	if (getgrouplist(pw->pw_name, pw->pw_gid, groups, &ngroups) < 0)
		return 0;
	for (i = 0; i < ngroups; i++)
		if (groups[i] == wheel)
			return 1;
	return 0;
}

#endif /* PRIV_SOCK_H */
