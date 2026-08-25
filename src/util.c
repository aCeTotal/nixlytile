/* See LICENSE.dwm file for copyright and license details. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <spawn.h>
#include <unistd.h>
#include <sys/wait.h>

extern char **environ;

#include "util.h"

/* When logging redirects stderr, die() must still print to the real terminal */
extern int log_stderr_fd;

void
die(const char *fmt, ...) {
	va_list ap;

	/* Restore real stderr so the user sees fatal errors on the terminal */
	if (log_stderr_fd >= 0)
		dup2(log_stderr_fd, STDERR_FILENO);

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);

	if (fmt[0] && fmt[strlen(fmt)-1] == ':') {
		fputc(' ', stderr);
		perror(NULL);
	} else {
		fputc('\n', stderr);
	}

	exit(1);
}

void *
ecalloc(size_t nmemb, size_t size)
{
	void *p;

	if (!(p = calloc(nmemb, size)))
		die("calloc:");
	return p;
}

int
fd_set_nonblock(int fd) {
	int flags = fcntl(fd, F_GETFL);
	if (flags < 0) {
		perror("fcntl(F_GETFL):");
		return -1;
	}
	if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
		perror("fcntl(F_SETFL):");
		return -1;
	}

	return 0;
}

char *
read_file_to_string(const char *path, size_t *out_len)
{
	FILE *fp;
	long fsize;
	char *buf;
	size_t nread;

	if (!path)
		return NULL;

	fp = fopen(path, "r");
	if (!fp)
		return NULL;

	fseek(fp, 0, SEEK_END);
	fsize = ftell(fp);
	fseek(fp, 0, SEEK_SET);

	if (fsize <= 0) {
		fclose(fp);
		return NULL;
	}

	buf = malloc((size_t)fsize + 1);
	if (!buf) {
		fclose(fp);
		return NULL;
	}

	nread = fread(buf, 1, (size_t)fsize, fp);
	fclose(fp);

	if (nread == 0) {
		free(buf);
		return NULL;
	}

	buf[nread] = '\0';
	if (out_len)
		*out_len = nread;
	return buf;
}

int
spawn_async_read(const char *cmd, pid_t *out_pid, int *out_fd)
{
	int pipefd[2];
	posix_spawn_file_actions_t fa;
	char *argv[] = { "/bin/sh", "-c", (char *)cmd, NULL };

	if (!cmd || !out_pid || !out_fd)
		return -1;

	if (pipe(pipefd) != 0)
		return -1;

	/* posix_spawn (vfork-based): fork() from the compositor copies the
	 * whole page-table incl. GPU mappings and stalls the event loop for
	 * tens of ms, felt as a cursor hitch on every popup data refresh */
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_addclose(&fa, pipefd[0]);
	posix_spawn_file_actions_adddup2(&fa, pipefd[1], STDOUT_FILENO);
	posix_spawn_file_actions_adddup2(&fa, pipefd[1], STDERR_FILENO);
	posix_spawn_file_actions_addclose(&fa, pipefd[1]);
	if (posix_spawn(out_pid, "/bin/sh", &fa, NULL, argv, environ) != 0) {
		posix_spawn_file_actions_destroy(&fa);
		close(pipefd[0]);
		close(pipefd[1]);
		*out_pid = -1;
		return -1;
	}
	posix_spawn_file_actions_destroy(&fa);

	close(pipefd[1]);
	*out_fd = pipefd[0];
	fcntl(*out_fd, F_SETFL, fcntl(*out_fd, F_GETFL) | O_NONBLOCK);
	return 0;
}
