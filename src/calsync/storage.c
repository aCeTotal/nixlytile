/* Data directories and atomic writes. */
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "calsync.h"

#define DIR_MODE 0700
#define APP_DIR  "nixlytile/calendar"

static void
mkdir_p(char *path)
{
	for (char *p = path + 1; *p; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		mkdir(path, DIR_MODE);
		*p = '/';
	}
	mkdir(path, DIR_MODE);
}

/* Relative XDG values are ignored. */
static void
resolve(char *dst, const char *env, const char *fallback)
{
	const char *base = getenv(env);
	const char *home = getenv("HOME");

	if (base && base[0] == '/')
		snprintf(dst, PATH_MAX, "%s/" APP_DIR, base);
	else
		snprintf(dst, PATH_MAX, "%s/%s/" APP_DIR, home ? home : "", fallback);
	mkdir_p(dst);
}

const char *
dir_data(void)
{
	static char path[PATH_MAX];

	if (!path[0])
		resolve(path, "XDG_DATA_HOME", ".local/share");
	return path;
}

const char *
dir_cache(void)
{
	static char path[PATH_MAX];

	if (!path[0])
		resolve(path, "XDG_CACHE_HOME", ".cache");
	return path;
}

const char *
dir_sources(void)
{
	static char path[PATH_MAX];

	if (path[0])
		return path;
	snprintf(path, sizeof path, "%s/src", dir_cache());
	mkdir(path, DIR_MODE);
	return path;
}

int
write_all(int fd, const char *buf, size_t len)
{
	while (len) {
		ssize_t n = write(fd, buf, len);

		if (n < 0)
			return -1;
		buf += n;
		len -= (size_t)n;
	}
	return 0;
}

int
fsync_path(const char *path)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	int rc;

	if (fd < 0)
		return -1;
	rc = fsync(fd);
	close(fd);
	return rc;
}

static int
sync_parent(const char *path)
{
	char dir[PATH_MAX];

	snprintf(dir, sizeof dir, "%s", path);
	*strrchr(dir, '/') = '\0';
	return fsync_path(dir);
}

int
file_replace(const char *path, const char *buf, size_t len, int mode)
{
	char tmp[PATH_MAX];
	int fd, failed;

	snprintf(tmp, sizeof tmp, "%s.tmp-%d", path, (int)getpid());
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
	if (fd < 0)
		return -1;
	failed = write_all(fd, buf, len) < 0 || fsync(fd) < 0;
	close(fd);
	if (failed || rename(tmp, path) < 0) {
		unlink(tmp);
		return -1;
	}
	return sync_parent(path);
}
