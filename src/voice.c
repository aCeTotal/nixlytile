#include "nixlytile.h"
#include "client.h"

#include <fcntl.h>
#include <spawn.h>

#define VOICE_BIN        "nixly-voice"
#define VOICE_LINE_MAX   512
#define VOICE_ARG_MAX    1024
#define VOICE_URL_MAX    (VOICE_ARG_MAX + 64)
#define VOICE_CMD_MAX    (VOICE_ARG_MAX + VOICE_URL_MAX + 4)
#define VOICE_RETRY_MS   1000
#define VOICE_RETRY_MAX  60000
#define VOICE_STABLE_MS  60000
#define VOICE_CALCULATOR "nixlykalk"
#define VOICE_BROWSER    "x-scheme-handler/http"
#define VOICE_FOLDER     "inode/directory"
#define VOICE_HOMEPAGE   "https://www.google.com"
#define VOICE_SEARCH     "https://www.google.com/search?q="

typedef struct {
	const char *verb;
	const char *object;
	void (*run)(const char *arg);
} VoiceCommand;

static pid_t voice_pid = -1;
static int voice_fd = -1;
static struct wl_event_source *voice_src;
static struct wl_event_source *voice_timer;
static uint64_t voice_since_ms;
static int voice_retry_ms = VOICE_RETRY_MS;
static char voice_line[VOICE_LINE_MAX];
static size_t voice_len;

static int
voice_unreserved(unsigned char c, const char *keep)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		(c >= '0' && c <= '9') || (c && strchr("-_.~", c)) || (c && strchr(keep, c));
}

/* Percent-encoding; never contains quotes. */
static void
voice_encode(const char *s, const char *keep, char out[VOICE_ARG_MAX])
{
	static const char hex[] = "0123456789ABCDEF";
	size_t n = 0;

	for (const unsigned char *p = (const unsigned char *)s; *p && n + 4 < VOICE_ARG_MAX; p++) {
		if (voice_unreserved(*p, keep)) {
			out[n++] = (char)*p;
			continue;
		}
		out[n++] = '%';
		out[n++] = hex[*p >> 4];
		out[n++] = hex[*p & 15];
	}
	out[n] = '\0';
}

/* Target always goes single-quoted. */
static void
voice_run(const char *exec, const char *target)
{
	char cmd[VOICE_CMD_MAX];

	if (target)
		snprintf(cmd, sizeof(cmd), "%s '%s'", exec, target);
	else
		snprintf(cmd, sizeof(cmd), "%s", exec);
	spawn_cmd(cmd);
}

/* $BROWSER first, as xdg-settings does. */
static int
voice_browser_exec(char exec[VOICE_ARG_MAX])
{
	const char *env = getenv("BROWSER");

	if (!env || !*env)
		return default_app_exec(VOICE_BROWSER, exec, VOICE_ARG_MAX);
	snprintf(exec, VOICE_ARG_MAX, "%.*s", (int)strcspn(env, ":"), env);
	return 1;
}

/* Most recently focused match. */
static Client *
voice_window(const AppNames *names)
{
	Client *c;

	wl_list_for_each(c, &fstack, flink)
		if (app_names_match(names, client_get_appid(c)))
			return c;
	return NULL;
}

/* Running apps get focus, not a twin. */
static int
voice_jump(const AppNames *names)
{
	Client *c = voice_window(names);
	Workspace *ws;

	if (!c)
		return 0;
	ws = c->column ? c->column->ws : NULL;
	if (ws && ws->mon && ws != ws->mon->active_ws) {
		workspace_switch(ws->mon, ws);
		arrange(ws->mon);
	}
	focusclient(c, 1);
	printstatus();
	return 1;
}

static void
voice_browser_names(AppNames *names)
{
	const char *env = getenv("BROWSER");
	char path[PATH_MAX];

	if (env && *env)
		app_names_add_program(names, env);
	else if (default_app_path(VOICE_BROWSER, path))
		app_names_from_desktop(path, names);
}

static void
voice_home(const char *arg)
{
	char exec[VOICE_ARG_MAX];
	char path[VOICE_ARG_MAX];
	char uri[VOICE_URL_MAX];
	const char *home = getenv("HOME");

	(void)arg;
	if (!default_app_exec(VOICE_FOLDER, exec, sizeof(exec)))
		snprintf(exec, sizeof(exec), "xdg-open");
	voice_encode(home ? home : "/", "/", path);
	snprintf(uri, sizeof(uri), "file://%s", path);
	voice_run(exec, uri);
}

static void
voice_calculator(const char *arg)
{
	AppNames names = { 0 };

	(void)arg;
	app_names_add_program(&names, VOICE_CALCULATOR);
	if (!voice_jump(&names))
		spawn_cmd(VOICE_CALCULATOR);
}

static void
voice_browser(const char *arg)
{
	char exec[VOICE_ARG_MAX];
	AppNames names = { 0 };

	(void)arg;
	voice_browser_names(&names);
	if (voice_jump(&names))
		return;
	if (voice_browser_exec(exec))
		voice_run(exec, NULL);
	else
		voice_run("xdg-open", VOICE_HOMEPAGE);
}

static void
voice_search(const char *query)
{
	char exec[VOICE_ARG_MAX];
	char encoded[VOICE_ARG_MAX];
	char url[VOICE_URL_MAX];

	if (!voice_browser_exec(exec))
		snprintf(exec, sizeof(exec), "xdg-open");
	voice_encode(query, "", encoded);
	snprintf(url, sizeof(url), "%s%s", VOICE_SEARCH, encoded);
	voice_run(exec, url);
}

/* Any app nixly_launcher lists. */
static void
voice_launch(const char *path)
{
	char exec[VOICE_ARG_MAX];
	AppNames names = { 0 };

	app_names_from_desktop(path, &names);
	if (voice_jump(&names))
		return;
	if (!desktop_exec(path, exec, sizeof(exec))) {
		wlr_log(WLR_ERROR, "voice: no Exec in %s", path);
		return;
	}
	spawn_cmd(exec);
}

static const VoiceCommand voice_commands[] = {
	{ "open", "home", voice_home },
	{ "open", "calculator", voice_calculator },
	{ "open", "browser", voice_browser },
	{ "search", NULL, voice_search },
	{ "launch", NULL, voice_launch },
};

/* "<verb> <object>", one per line. */
static void
voice_dispatch(char *line)
{
	char *arg = strchr(line, ' ');

	if (!arg)
		return;
	*arg++ = '\0';
	for (size_t i = 0; i < LENGTH(voice_commands); i++) {
		const VoiceCommand *c = &voice_commands[i];

		if (strcmp(line, c->verb) || (c->object && strcmp(arg, c->object)))
			continue;
		c->run(arg);
		return;
	}
	wlr_log(WLR_ERROR, "voice: unknown command '%s %s'", line, arg);
}

static void
voice_consume(const char *chunk, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		if (chunk[i] != '\n') {
			if (voice_len < sizeof(voice_line) - 1)
				voice_line[voice_len++] = chunk[i];
			continue;
		}
		voice_line[voice_len] = '\0';
		voice_dispatch(voice_line);
		voice_len = 0;
	}
}

/* Restart with exponential backoff. */
static void
voice_lost(void)
{
	if (voice_src)
		wl_event_source_remove(voice_src);
	if (voice_fd >= 0)
		close(voice_fd);
	voice_src = NULL;
	voice_fd = -1;
	voice_pid = -1;
	voice_len = 0;
	if (monotonic_msec() - voice_since_ms >= VOICE_STABLE_MS)
		voice_retry_ms = VOICE_RETRY_MS;
	wl_event_source_timer_update(voice_timer, voice_retry_ms);
	voice_retry_ms = MIN(voice_retry_ms * 2, VOICE_RETRY_MAX);
}

static int
voice_readable(int fd, uint32_t mask, void *data)
{
	char chunk[VOICE_LINE_MAX];
	ssize_t n;

	(void)mask;
	(void)data;
	while ((n = read(fd, chunk, sizeof(chunk))) > 0)
		voice_consume(chunk, (size_t)n);
	if (n == 0 || (errno != EAGAIN && errno != EINTR))
		voice_lost();
	return 0;
}

/* Stdout is the command pipe. */
static int
voice_spawn(int out)
{
	char *const argv[] = { VOICE_BIN, NULL };
	posix_spawn_file_actions_t fa;
	posix_spawnattr_t at;
	sigset_t none, all;
	int r;

	sigemptyset(&none);
	sigfillset(&all);
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_addopen(&fa, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
	posix_spawn_file_actions_adddup2(&fa, out, STDOUT_FILENO);
	posix_spawn_file_actions_addclosefrom_np(&fa, STDERR_FILENO + 1);
	posix_spawnattr_init(&at);
	posix_spawnattr_setsigmask(&at, &none);
	posix_spawnattr_setsigdefault(&at, &all);
	posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSID | POSIX_SPAWN_SETSIGMASK |
			POSIX_SPAWN_SETSIGDEF);
	r = posix_spawnp(&voice_pid, VOICE_BIN, &fa, &at, argv, environ);
	posix_spawnattr_destroy(&at);
	posix_spawn_file_actions_destroy(&fa);
	return r;
}

static int
voice_start_child(void *data)
{
	int pipefd[2];
	int err;

	(void)data;
	voice_since_ms = monotonic_msec();
	if (pipe2(pipefd, O_CLOEXEC) != 0) {
		voice_lost();
		return 0;
	}
	fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
	err = voice_spawn(pipefd[1]);
	close(pipefd[1]);
	voice_fd = pipefd[0];
	if (err) {
		wlr_log(WLR_ERROR, "voice: cannot start %s: %s", VOICE_BIN, strerror(err));
		voice_lost();
		return 0;
	}
	voice_src = wl_event_loop_add_fd(event_loop, voice_fd, WL_EVENT_READABLE,
			voice_readable, NULL);
	return 0;
}

void
voice_start(void)
{
	voice_timer = wl_event_loop_add_timer(event_loop, voice_start_child, NULL);
	voice_start_child(NULL);
}

void
voice_stop(void)
{
	if (voice_pid > 0)
		kill(voice_pid, SIGTERM);
	if (voice_src)
		wl_event_source_remove(voice_src);
	if (voice_timer)
		wl_event_source_remove(voice_timer);
	voice_src = voice_timer = NULL;
}
