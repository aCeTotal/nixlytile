#ifndef NIXLYTILE_SPAWN_H
#define NIXLYTILE_SPAWN_H

/*
 * Run a command to completion and report whether it succeeded.
 *
 * Reliable even though the compositor's SIGCHLD handler reaps with
 * waitpid(-1, WNOHANG): the exit status is passed back out of band, so it
 * cannot be lost to that race the way system()'s and pclose()'s are.
 *
 * Returns 0 if the command exited with status 0, -1 otherwise.
 * stdout and stderr are sent to /dev/null.
 */
int run_cmd(const char *const argv[]);

#endif /* NIXLYTILE_SPAWN_H */
