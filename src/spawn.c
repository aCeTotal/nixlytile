#include "nixlytile.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <unistd.h>

#include "launchboost.h"
#include "run_cmd.h"

#define CMD_MAX 4096
#define WORDS_MAX 64
#define REPORT_FD 3
#define DGPU_WRAPPER "nixly-dgpu"

extern char **environ;

/* Child gets report_fd as REPORT_FD. */
typedef struct {
	int report_fd;
} SpawnOpts;

/* No page-table copy before exec. */
static pid_t
spawn_child(char *const argv[], const SpawnOpts *o)
{
	posix_spawn_file_actions_t fa;
	posix_spawnattr_t at;
	sigset_t none, dflt;
	const char *home = getenv("HOME");
	pid_t pid = -1;

	posix_spawn_file_actions_init(&fa);
	posix_spawnattr_init(&at);
	posix_spawn_file_actions_addopen(&fa, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
	if (o->report_fd >= 0) {
		posix_spawn_file_actions_addopen(&fa, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
		posix_spawn_file_actions_adddup2(&fa, STDOUT_FILENO, STDERR_FILENO);
		posix_spawn_file_actions_adddup2(&fa, o->report_fd, REPORT_FD);
	}
	posix_spawn_file_actions_addclosefrom_np(&fa, o->report_fd >= 0 ? REPORT_FD + 1 : 3);
	posix_spawn_file_actions_addchdir_np(&fa, home && *home ? home : "/");
	sigemptyset(&none);
	sigemptyset(&dflt);
	sigaddset(&dflt, SIGCHLD);
	sigaddset(&dflt, SIGINT);
	sigaddset(&dflt, SIGTERM);
	sigaddset(&dflt, SIGPIPE);
	posix_spawnattr_setsigmask(&at, &none);
	posix_spawnattr_setsigdefault(&at, &dflt);
	posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSID | POSIX_SPAWN_SETSIGMASK |
		POSIX_SPAWN_SETSIGDEF);
	if (posix_spawnp(&pid, argv[0], &fa, &at, argv, environ) != 0)
		pid = -1;
	posix_spawnattr_destroy(&at);
	posix_spawn_file_actions_destroy(&fa);
	return pid;
}

/* Pipe status, safe from reaper. */
int
run_cmd(const char *const argv[])
{
	static const char script[] = "\"$@\" 3>&- && printf 1 >&3";
	char *sh_argv[WORDS_MAX + 4] = { "/bin/sh", "-c", (char *)script, "sh" };
	int n = 4, pipefd[2];
	char ok = 0;
	ssize_t got;
	SpawnOpts o;

	for (int i = 0; argv[i] && n < WORDS_MAX + 3; i++)
		sh_argv[n++] = (char *)argv[i];
	sh_argv[n] = NULL;
	if (pipe2(pipefd, O_CLOEXEC) < 0)
		return -1;
	o.report_fd = pipefd[1];
	if (spawn_child(sh_argv, &o) < 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		return -1;
	}
	close(pipefd[1]);
	while ((got = read(pipefd[0], &ok, 1)) < 0 && errno == EINTR)
		;
	close(pipefd[0]);
	return got == 1 && ok == '1' ? 0 : -1;
}

static int
on_path(const char *name)
{
	char path[CMD_MAX], file[PATH_MAX], *save = NULL;
	const char *env = getenv("PATH");
	int found = 0;

	snprintf(path, sizeof(path), "%s", env ? env : "");
	for (char *dir = strtok_r(path, ":", &save); dir && !found; dir = strtok_r(NULL, ":", &save)) {
		snprintf(file, sizeof(file), "%s/%s", dir, name);
		found = access(file, X_OK) == 0;
	}
	return found;
}

const char *
steam_binary(void)
{
	static const char *bin;

	if (!bin)
		bin = on_path("nixly_steam") ? "nixly_steam" : "steam";
	return bin;
}

static int
dgpu_wrapper(void)
{
	static int found = -1;

	if (found < 0)
		found = on_path(DGPU_WRAPPER);
	return found;
}

static int
shell_meta(const char *s)
{
	return strpbrk(s, "|&;$`()<>{}~*?[]'\"\\") != NULL;
}

/* Splits meta-free line in place. */
static int
split_words(char *line, char **argv)
{
	int n = 0;
	char *save = NULL;

	for (char *w = strtok_r(line, " \t", &save); w && n < WORDS_MAX - 1;
	     w = strtok_r(NULL, " \t", &save))
		argv[n++] = w;
	argv[n] = NULL;
	return n;
}

/* Steam and dGPU launch rewrites. */
static void
launch_line(const char *cmd, char *out, size_t len)
{
	const char *uri = strstr(cmd, "steam://");

	if (is_steam_cmd(cmd))
		snprintf(out, len, "%s%s%s", steam_binary(), uri ? " " : "", uri ? uri : "");
	else if (integrated_gpu_idx >= 0 && !nvidia_render_primary && !shell_meta(cmd) &&
			should_use_dgpu(cmd) && dgpu_wrapper())
		snprintf(out, len, DGPU_WRAPPER " %s", cmd);
	else
		snprintf(out, len, "%s", cmd);
}

pid_t
spawn_cmd(const char *cmd)
{
	char line[CMD_MAX], words[CMD_MAX], script[CMD_MAX + 8];
	char *argv[WORDS_MAX];
	const char *shell = getenv("SHELL");
	SpawnOpts o = { -1 };
	pid_t pid = -1;

	if (!cmd || !cmd[0])
		return -1;
	launchboost_kick();
	launch_line(cmd, line, sizeof(line));
	snprintf(words, sizeof(words), "%s", line);
	if (!shell_meta(line) && split_words(words, argv) > 0)
		pid = spawn_child(argv, &o);
	if (pid < 0) {
		char *sh_argv[] = { (char *)(shell && *shell ? shell : "/bin/sh"), "-c", script, NULL };

		snprintf(script, sizeof(script), "exec %s", line);
		pid = spawn_child(sh_argv, &o);
	}
	if (pid > 0 && selmon)
		pending_launch_add(pid, selmon->tagset[selmon->seltags], selmon->nx_output->name);
	return pid;
}

/* Matches first word's program name. */
int
should_use_dgpu(const char *cmd)
{
	char word[256];
	const char *base;
	size_t n = cmd ? strcspn(cmd, " \t") : 0;

	if (n == 0 || n >= sizeof(word))
		return 0;
	memcpy(word, cmd, n);
	word[n] = '\0';
	base = strrchr(word, '/');
	base = base ? base + 1 : word;
	for (int i = 0; dgpu_programs[i]; i++)
		if (!strcasecmp(base, dgpu_programs[i]))
			return 1;
	return 0;
}
