/* Starting the sync helper. */
#include "nixlytile.h"
#include "calstore/calstore.h"

#include <fcntl.h>
#include <libgen.h>
#include <signal.h>
#include <spawn.h>

#define CALSYNC_BIN "nixly-calsync"

extern char **environ;

/* Beside compositor, else PATH. */
static const char *
calsync_path(void)
{
	static char path[PATH_MAX];
	char exe[PATH_MAX];
	ssize_t n;

	if (path[0])
		return path;
	n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
	if (n > 0) {
		exe[n] = '\0';
		snprintf(path, sizeof(path), "%s/" CALSYNC_BIN, dirname(exe));
	}
	if (n <= 0 || access(path, X_OK) != 0)
		snprintf(path, sizeof(path), CALSYNC_BIN);
	return path;
}

static pid_t
spawn_calsync(char *const argv[], int in, int out)
{
	posix_spawn_file_actions_t fa;
	posix_spawnattr_t at;
	sigset_t none, dflt;
	pid_t pid = -1;

	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_adddup2(&fa, in, STDIN_FILENO);
	posix_spawn_file_actions_adddup2(&fa, out, STDOUT_FILENO);
	posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, "/dev/null",
			O_WRONLY, 0);
	posix_spawn_file_actions_addclosefrom_np(&fa, STDERR_FILENO + 1);
	posix_spawnattr_init(&at);
	sigemptyset(&none);
	sigemptyset(&dflt);
	sigaddset(&dflt, SIGCHLD);
	sigaddset(&dflt, SIGPIPE);
	posix_spawnattr_setsigmask(&at, &none);
	posix_spawnattr_setsigdefault(&at, &dflt);
	posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSID |
			POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
	if (posix_spawnp(&pid, argv[0], &fa, &at, argv, environ) != 0)
		pid = -1;
	posix_spawnattr_destroy(&at);
	posix_spawn_file_actions_destroy(&fa);
	return pid;
}

Launch
launch_calsync(const JobSpec *s)
{
	char *argv[] = { (char *)calsync_path(), (char *)s->cmd,
		(char *)s->arg, NULL };
	Launch l = { -1, -1 };
	int in[2], out[2];

	if (pipe2(in, O_CLOEXEC) < 0)
		return l;
	if (pipe2(out, O_CLOEXEC) < 0) {
		close(in[0]);
		close(in[1]);
		return l;
	}
	l.pid = spawn_calsync(argv, in[0], out[1]);
	close(in[0]);
	close(out[1]);
	if (l.pid > 0 && s->payload)
		write_all(in[1], s->payload, strlen(s->payload));
	close(in[1]);
	if (l.pid > 0)
		l.fd = out[0];
	else
		close(out[0]);
	return l;
}
