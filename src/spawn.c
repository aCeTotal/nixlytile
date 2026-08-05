#include <errno.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include "spawn.h"

/*
 * system() cannot be trusted in this process: handlesig() reaps with
 * waitpid(-1, WNOHANG) on every SIGCHLD, so it races system()'s own wait and
 * system() then returns -1/ECHILD for a command that ran perfectly. Every
 * caller that gated a tuning step on that return value silently skipped it.
 *
 * So the status is passed back out of band. We fork a helper which forks the
 * real command: the helper is the command's own parent, so its waitpid()
 * cannot be stolen by our SIGCHLD handler, and it reports the result over a
 * pipe. The handler is welcome to reap the helper afterwards — we never wait
 * for it, we wait for the pipe.
 */
int
run_cmd(const char *const argv[])
{
	int pipefd[2];

	if (pipe(pipefd) < 0)
		return -1;

	pid_t helper = fork();
	if (helper < 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		return -1;
	}

	if (helper == 0) {
		close(pipefd[0]);

		pid_t child = fork();
		if (child == 0) {
			int devnull = open("/dev/null", O_WRONLY);
			if (devnull >= 0) {
				dup2(devnull, STDOUT_FILENO);
				dup2(devnull, STDERR_FILENO);
				if (devnull > STDERR_FILENO)
					close(devnull);
			}
			execvp(argv[0], (char *const *)argv);
			_exit(127);
		}

		unsigned char ok = 0;
		if (child > 0) {
			int status;
			while (waitpid(child, &status, 0) < 0 && errno == EINTR)
				;
			ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
		}
		while (write(pipefd[1], &ok, 1) < 0 && errno == EINTR)
			;
		_exit(0);
	}

	close(pipefd[1]);

	unsigned char ok = 0;
	ssize_t n;
	while ((n = read(pipefd[0], &ok, 1)) < 0 && errno == EINTR)
		;
	close(pipefd[0]);

	return (n == 1 && ok) ? 0 : -1;
}
