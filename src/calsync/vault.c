/* Account vault sealed by systemd-creds. */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "calsync.h"

#define CREDS           "systemd-creds"
#define CRED_NAME       "--name=nixlytile-calendar"
#define VAULT_FILE      "vault.cred"
#define LOCK_FILE       "vault.lock"
#define LISTING_FILE    "accounts"
#define PRIVATE_MODE    0600
#define LISTING_LINE    256

typedef struct {
	pid_t pid;
	int fd;
} Child;

static void
data_path(char *dst, const char *name)
{
	snprintf(dst, PATH_MAX, "%s/%s", dir_data(), name);
}

int
vault_lock(void)
{
	char path[PATH_MAX];
	int fd;

	data_path(path, LOCK_FILE);
	fd = open(path, O_RDONLY | O_CREAT | O_CLOEXEC, PRIVATE_MODE);
	if (fd < 0)
		return -1;
	if (flock(fd, LOCK_EX) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

void
vault_unlock(int fd)
{
	close(fd);
}

static void
wipe(Plaintext p)
{
	explicit_bzero(p.buf, p.len);
	free(p.buf);
}

/* Pipe target stdio, rest null. */
static Child
spawn_piped(char *const argv[], int target)
{
	posix_spawn_file_actions_t fa;
	Child c = { -1, -1 };
	int fd[2], theirs;

	if (pipe2(fd, O_CLOEXEC) < 0)
		return c;
	theirs = target == STDIN_FILENO ? fd[0] : fd[1];
	c.fd = target == STDIN_FILENO ? fd[1] : fd[0];
	posix_spawn_file_actions_init(&fa);
	for (int i = STDIN_FILENO; i <= STDERR_FILENO; i++) {
		if (i == target)
			posix_spawn_file_actions_adddup2(&fa, theirs, i);
		else
			posix_spawn_file_actions_addopen(&fa, i, "/dev/null", O_RDWR, 0);
	}
	if (posix_spawnp(&c.pid, argv[0], &fa, NULL, argv, environ))
		c.pid = -1;
	posix_spawn_file_actions_destroy(&fa);
	close(theirs);
	if (c.pid < 0)
		close(c.fd);
	return c;
}

static int
reap(pid_t pid)
{
	int status;

	if (waitpid(pid, &status, 0) < 0)
		return -1;
	return WIFEXITED(status) && !WEXITSTATUS(status) ? 0 : -1;
}

/* Plaintext is shorter than ciphertext. */
static Plaintext
decrypt(char *path, size_t cap)
{
	char *argv[] = { CREDS, "decrypt", "--user", CRED_NAME, path, "-", NULL };
	Child c = spawn_piped(argv, STDOUT_FILENO);
	Plaintext p = { NULL, 0 };
	ssize_t n = 0;

	if (c.pid < 0)
		return p;
	p.buf = xrealloc(NULL, cap + 1);
	while (p.len < cap && (n = read(c.fd, p.buf + p.len, cap - p.len)) > 0)
		p.len += (size_t)n;
	p.buf[p.len] = '\0';
	close(c.fd);
	if (reap(c.pid) == 0 && n >= 0)
		return p;
	wipe(p);
	return (Plaintext){ NULL, 0 };
}

static int
encrypt_to(char *path, Plaintext p)
{
	char *argv[] = { CREDS, "encrypt", "--user", CRED_NAME, "-", path, NULL };
	Child c = spawn_piped(argv, STDIN_FILENO);
	int failed;

	if (c.pid < 0)
		return -1;
	failed = write_all(c.fd, p.buf, p.len) < 0;
	close(c.fd);
	return reap(c.pid) < 0 || failed ? -1 : 0;
}

int
vault_load(Vault *v)
{
	char path[PATH_MAX];
	struct stat st;
	Plaintext p;

	memset(v, 0, sizeof *v);
	data_path(path, VAULT_FILE);
	if (stat(path, &st) < 0)
		return errno == ENOENT ? 0 : -1;
	p = decrypt(path, (size_t)st.st_size);
	if (!p.buf)
		return -1;
	vault_parse(v, p.buf);
	wipe(p);
	return 0;
}

static int
writable(const Account *a)
{
	if (!strcmp(a->provider, "google") || !strcmp(a->provider, "microsoft"))
		return 1;
	return strcmp(a->provider, "ics") && a->todo_url[0];
}

static int
write_listing(const Vault *v)
{
	char buf[ACCOUNT_MAX * LISTING_LINE], path[PATH_MAX];
	size_t n = 0;

	for (int i = 0; i < v->nacc; i++) {
		const Account *a = &v->acc[i];

		n += (size_t)snprintf(buf + n, sizeof buf - n, "%s\t%s\t%s\t%d\t%d\n",
				a->id, a->provider, a->label, a->ncals, writable(a));
	}
	snprintf(path, sizeof path, "%s/" LISTING_FILE, dir_cache());
	return file_replace(path, buf, n, PRIVATE_MODE);
}

int
vault_save(const Vault *v)
{
	char path[PATH_MAX], tmp[PATH_MAX];
	Plaintext p = vault_format(v);
	int failed;

	data_path(path, VAULT_FILE);
	snprintf(tmp, sizeof tmp, "%s/" VAULT_FILE ".tmp-%d", dir_data(), (int)getpid());
	failed = encrypt_to(tmp, p) < 0;
	wipe(p);
	if (failed || fsync_path(tmp) < 0 || rename(tmp, path) < 0) {
		unlink(tmp);
		return -1;
	}
	if (fsync_path(dir_data()) < 0)
		return -1;
	return write_listing(v);
}
