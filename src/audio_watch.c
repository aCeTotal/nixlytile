/*
 * audio_watch.c — pw-metadata monitor so the status bar's volume module
 * flips the speaker/headset icon the moment the default audio sink
 * changes, instead of waiting for the next 45s status refresh.
 */
#include "nixlytile.h"

#include <fcntl.h>
#include <signal.h>

static struct wl_event_source *audio_watch_src;
static struct wl_event_source *audio_watch_retry;
static int audio_watch_fd = -1;
static pid_t audio_watch_pid = -1;
/* pw-metadata dumps the current defaults on startup, so a healthy child
 * always produces output. A child that dies without ever writing means
 * the binary/daemon is missing — give up after a few tries. */
static int audio_watch_got_data;
static int audio_watch_silent_deaths;

/* WirePlumber may still be moving streams when the default flips. */
#define AUDIO_WATCH_SETTLE_MS 400
#define AUDIO_WATCH_RETRY_MS 5000
#define AUDIO_WATCH_MAX_SILENT_DEATHS 5

static int audio_watch_spawn(void *data);

static void
audio_watch_stop(void)
{
	if (audio_watch_src) {
		wl_event_source_remove(audio_watch_src);
		audio_watch_src = NULL;
	}
	if (audio_watch_fd >= 0) {
		close(audio_watch_fd);
		audio_watch_fd = -1;
	}
	if (audio_watch_pid > 0) {
		kill(audio_watch_pid, SIGTERM);
		audio_watch_pid = -1;
	}
}

static int
audio_watch_cb(int fd, uint32_t mask, void *data)
{
	char buf[2048];
	ssize_t n = -1;
	int changed = 0;

	(void)data;

	while ((n = read(fd, buf, sizeof(buf) - 1)) > 0) {
		buf[n] = '\0';
		audio_watch_got_data = 1;
		if (strstr(buf, "audio.sink"))
			changed = 1;
	}

	if (changed) {
		volume_invalidate_cache(0);
		volume_invalidate_cache(1);
		set_status_task_due(refreshstatusvolume,
				monotonic_msec() + AUDIO_WATCH_SETTLE_MS);
	}

	if ((mask & WL_EVENT_HANGUP) || n == 0) {
		if (audio_watch_got_data)
			audio_watch_silent_deaths = 0;
		else
			audio_watch_silent_deaths++;
		audio_watch_stop();
		if (audio_watch_silent_deaths < AUDIO_WATCH_MAX_SILENT_DEATHS
				&& audio_watch_retry)
			wl_event_source_timer_update(audio_watch_retry,
					AUDIO_WATCH_RETRY_MS);
	}
	return 0;
}

static int
audio_watch_spawn(void *data)
{
	int fds[2];
	pid_t pid;

	(void)data;

	if (audio_watch_fd >= 0)
		return 0;
	if (pipe2(fds, O_CLOEXEC) < 0)
		return 0;

	pid = fork();
	if (pid < 0) {
		close(fds[0]);
		close(fds[1]);
		return 0;
	}
	if (pid == 0) {
		int devnull = open("/dev/null", O_RDWR);
		dup2(fds[1], STDOUT_FILENO);
		if (devnull >= 0) {
			dup2(devnull, STDIN_FILENO);
			dup2(devnull, STDERR_FILENO);
			close(devnull);
		}
		close(fds[0]);
		close(fds[1]);
		setsid();
		execlp("pw-metadata", "pw-metadata", "-m", "default", NULL);
		_exit(127);
	}

	close(fds[1]);
	fcntl(fds[0], F_SETFL, O_NONBLOCK);
	audio_watch_fd = fds[0];
	audio_watch_pid = pid;
	audio_watch_got_data = 0;
	audio_watch_src = wl_event_loop_add_fd(event_loop, audio_watch_fd,
			WL_EVENT_READABLE, audio_watch_cb, NULL);
	return 0;
}

void
audio_watch_setup(void)
{
	if (audio_watch_fd >= 0)
		return;
	if (!audio_watch_retry)
		audio_watch_retry = wl_event_loop_add_timer(event_loop,
				audio_watch_spawn, NULL);
	audio_watch_spawn(NULL);
}

void
audio_watch_cleanup(void)
{
	audio_watch_stop();
	if (audio_watch_retry) {
		wl_event_source_remove(audio_watch_retry);
		audio_watch_retry = NULL;
	}
}
