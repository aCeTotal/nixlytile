#include "nixlytile.h"
#include "mpv_launcher.h"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

/* ── State ─────────────────────────────────────────────────────────── */

static pid_t  mpv_pid          = -1;
static int    mpv_pidfd        = -1;
static int    mpv_ipc_fd       = -1;
static int    mpv_media_id     = -1;
static double mpv_last_pos     = 0.0;
static double mpv_resume_target = 0.0;  /* seek target to apply after file-loaded */
static int    mpv_resume_applied = 0;
static int    mpv_eof_reached  = 0;
static char   mpv_sock_path[160];
static char   mpv_rx_buf[4096];
static size_t mpv_rx_len       = 0;

static struct wl_event_source *mpv_pidfd_src = NULL;
static struct wl_event_source *mpv_ipc_src   = NULL;

/* ── Forward decls ─────────────────────────────────────────────────── */

static int mpv_on_pidfd(int fd, uint32_t mask, void *data);
static int mpv_on_ipc(int fd, uint32_t mask, void *data);
static void mpv_cleanup(void);
static int  pidfd_open_compat(pid_t pid);

/* ── pidfd syscall wrapper (Linux 5.3+) ────────────────────────────── */

static int
pidfd_open_compat(pid_t pid)
{
#ifdef SYS_pidfd_open
	return (int)syscall(SYS_pidfd_open, pid, 0);
#else
	(void)pid;
	errno = ENOSYS;
	return -1;
#endif
}

/* ── IPC helpers ───────────────────────────────────────────────────── */

int
mpv_launcher_send_cmd(const char *json)
{
	if (mpv_ipc_fd < 0 || !json) return -1;

	char buf[512];
	int n = snprintf(buf, sizeof(buf), "%s\n", json);
	if (n <= 0 || (size_t)n >= sizeof(buf)) return -1;

	ssize_t off = 0;
	while (off < n) {
		ssize_t w = write(mpv_ipc_fd, buf + off, (size_t)(n - off));
		if (w < 0) {
			if (errno == EINTR) continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				/* IPC socket full — drop command rather than block compositor */
				wlr_log(WLR_ERROR, "mpv IPC write would block, dropping");
				return -1;
			}
			wlr_log(WLR_ERROR, "mpv IPC write failed: %s", strerror(errno));
			return -1;
		}
		off += w;
	}
	return 0;
}

void mpv_launcher_toggle_pause(void)
{
	mpv_launcher_send_cmd("{\"command\":[\"cycle\",\"pause\"]}");
}

void mpv_launcher_seek_relative(double seconds)
{
	char j[128];
	snprintf(j, sizeof(j), "{\"command\":[\"seek\",%.3f,\"relative\"]}", seconds);
	mpv_launcher_send_cmd(j);
}

void mpv_launcher_cycle_audio(void)
{
	mpv_launcher_send_cmd("{\"command\":[\"cycle\",\"aid\"]}");
}

void mpv_launcher_cycle_sub(void)
{
	mpv_launcher_send_cmd("{\"command\":[\"cycle\",\"sid\"]}");
}

void mpv_launcher_volume_delta(double delta)
{
	char j[128];
	snprintf(j, sizeof(j), "{\"command\":[\"add\",\"volume\",%.1f]}", delta);
	mpv_launcher_send_cmd(j);
}

/* ── Parse time-pos from mpv property-change events ────────────────── */

static void mpv_handle_event_line(const char *line);

static void
mpv_handle_line(const char *line)
{
	if (!strstr(line, "\"event\":\"property-change\"")) {
		mpv_handle_event_line(line);
		return;
	}

	const char *d = strstr(line, "\"data\":");
	if (!d) return;

	if (strstr(line, "\"name\":\"time-pos\"")) {
		const char *p = d + 7;
		while (*p == ' ') p++;
		if (*p == 'n') return;  /* null (paused before first frame) */
		char *end;
		double v = strtod(p, &end);
		if (end != p && v >= 0.0)
			mpv_last_pos = v;
		return;
	}

	if (strstr(line, "\"name\":\"pause\"")) {
		const char *p = d + 7;
		while (*p == ' ') p++;
		/* Any pause=true that wasn't user-initiated — force resume.
		 * mpv can self-pause on first frame / cache-idle with some
		 * profile+cache combos; the user never requested it. */
		if (strncmp(p, "true", 4) == 0) {
			mpv_launcher_send_cmd(
				"{\"command\":[\"set_property\",\"pause\",false]}");
		}
		return;
	}

	if (strstr(line, "\"name\":\"eof-reached\"")) {
		const char *p = d + 7;
		while (*p == ' ') p++;
		if (strncmp(p, "true", 4) == 0)
			mpv_eof_reached = 1;
		return;
	}
}

static void
mpv_handle_event_line(const char *line)
{
	/* file-loaded: apply resume seek (if any) via IPC rather than --start.
	 * Passing --start=N to the mpv CLI puts it in a "seek on load" state
	 * that stalls on first frame until the user seeks manually — the
	 * whole reason this function exists. An IPC seek after file-loaded
	 * avoids that stall path.
	 *
	 * playback-restart: fires after load and after every seek. Force
	 * unpause in case mpv self-paused on first frame (cache states,
	 * profile quirks). Also do a tiny frame-step nudge so the decoder
	 * emits the first frame immediately instead of waiting for the
	 * cache-pause-wait threshold. */
	if (strstr(line, "\"event\":\"file-loaded\"")) {
		/*
		 * Always seek on file-loaded, even for fresh playback (target=0).
		 * A zero-position exact seek forces the decoder to emit the first
		 * frame AND starts mpv's playback clock — without it mpv sits on
		 * the first frame with a stopped clock until the user manually
		 * seeks forward.
		 */
		char j[128];
		double target = (!mpv_resume_applied && mpv_resume_target > 0.5)
			? mpv_resume_target : 0.0;
		snprintf(j, sizeof(j),
			"{\"command\":[\"seek\",%.3f,\"absolute\",\"exact\"]}",
			target);
		mpv_launcher_send_cmd(j);
		mpv_resume_applied = 1;
		mpv_launcher_send_cmd(
			"{\"command\":[\"set_property\",\"pause\",false]}");
		return;
	}
	if (strstr(line, "\"event\":\"playback-restart\"")) {
		mpv_launcher_send_cmd(
			"{\"command\":[\"set_property\",\"pause\",false]}");
	}
}

static int
mpv_on_ipc(int fd, uint32_t mask, void *data)
{
	(void)data;

	if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
		/* mpv closed the socket — exit will be caught by pidfd watcher */
		return 0;
	}

	for (;;) {
		if (mpv_rx_len >= sizeof(mpv_rx_buf) - 1) {
			/* Overflow — discard partial line */
			mpv_rx_len = 0;
		}
		ssize_t n = read(fd, mpv_rx_buf + mpv_rx_len,
		                 sizeof(mpv_rx_buf) - 1 - mpv_rx_len);
		if (n == 0) return 0;
		if (n < 0) {
			if (errno == EINTR) continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK) break;
			return 0;
		}
		mpv_rx_len += (size_t)n;
		mpv_rx_buf[mpv_rx_len] = '\0';

		/* Process complete lines */
		char *start = mpv_rx_buf;
		char *nl;
		while ((nl = memchr(start, '\n', mpv_rx_len - (size_t)(start - mpv_rx_buf)))) {
			*nl = '\0';
			mpv_handle_line(start);
			start = nl + 1;
		}
		size_t leftover = mpv_rx_len - (size_t)(start - mpv_rx_buf);
		if (leftover && start != mpv_rx_buf)
			memmove(mpv_rx_buf, start, leftover);
		mpv_rx_len = leftover;
	}
	return 0;
}

/* ── Exit handling ─────────────────────────────────────────────────── */

static int
mpv_on_pidfd(int fd, uint32_t mask, void *data)
{
	(void)fd; (void)mask; (void)data;

	int status;
	waitpid(mpv_pid, &status, WNOHANG);
	wlr_log(WLR_INFO, "mpv exited (pid=%d) eof=%d pos=%.1fs",
	        mpv_pid, mpv_eof_reached, mpv_last_pos);

	if (mpv_media_id >= 0) {
		if (mpv_eof_reached)
			save_resume_position(mpv_media_id, 0.0);
		else if (mpv_last_pos > 1.0)
			save_resume_position(mpv_media_id, mpv_last_pos);
	}

	mpv_cleanup();

	hide_playback_osd();
	audio_track_count = 0;
	subtitle_track_count = 0;
	playback_state = PLAYBACK_IDLE;
	nixly_cursor_set_xcursor("default");

	media_playback_ended();

	if (selmon)
		wlr_output_schedule_frame(selmon->wlr_output);
	return 0;
}

static void
mpv_cleanup(void)
{
	if (mpv_ipc_src) { wl_event_source_remove(mpv_ipc_src); mpv_ipc_src = NULL; }
	if (mpv_pidfd_src) { wl_event_source_remove(mpv_pidfd_src); mpv_pidfd_src = NULL; }
	if (mpv_ipc_fd >= 0) { close(mpv_ipc_fd); mpv_ipc_fd = -1; }
	if (mpv_pidfd >= 0)  { close(mpv_pidfd);  mpv_pidfd  = -1; }
	if (mpv_sock_path[0]) { unlink(mpv_sock_path); mpv_sock_path[0] = '\0'; }
	mpv_pid            = -1;
	mpv_media_id       = -1;
	mpv_last_pos       = 0.0;
	mpv_resume_target  = 0.0;
	mpv_resume_applied = 0;
	mpv_eof_reached    = 0;
	mpv_rx_len         = 0;
}

/* ── IPC socket connect (retry loop, mpv creates it on startup) ──────
 *
 * mpv can't initialize its Wayland surface until the compositor processes
 * its Wayland requests.  A plain sleep loop would block the compositor
 * thread for up to 5s → mpv's `wl_display_roundtrip()` hangs → mpv never
 * opens the IPC socket → we time out and kill it → user sees nothing.
 *
 * Instead we drive `wl_event_loop_dispatch()` during each wait slice so
 * mpv's Wayland traffic gets serviced while we poll for the socket.
 */
static int
mpv_ipc_connect(const char *path)
{
	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

	for (int tries = 0; tries < 250; tries++) {  /* up to ~5s */
		int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
		if (fd < 0) return -1;

		if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0)
			return fd;

		close(fd);

		/* Child might have died already */
		int status;
		if (waitpid(mpv_pid, &status, WNOHANG) == mpv_pid) {
			wlr_log(WLR_ERROR, "mpv exited before IPC socket ready");
			mpv_pid = -1;
			return -1;
		}

		/* Pump the compositor event loop so mpv's Wayland init can
		 * make progress. wl_event_loop_dispatch only reads requests —
		 * without flush_clients(), our replies (wl_registry globals,
		 * xdg_surface.configure etc) never leave the compositor and
		 * mpv hangs on wl_display_roundtrip until we kill it. */
		if (event_loop) {
			wl_event_loop_dispatch(event_loop, 20);
			if (dpy)
				wl_display_flush_clients(dpy);
		} else {
			struct timespec ts = { .tv_sec = 0, .tv_nsec = 20 * 1000 * 1000 };
			nanosleep(&ts, NULL);
		}
	}
	wlr_log(WLR_ERROR, "mpv IPC connect timeout: %s", path);
	return -1;
}

/* ── Start mpv ─────────────────────────────────────────────────────── */

int
mpv_launcher_start(const char *url, double resume_pos, int media_id)
{
	if (!url || mpv_pid > 0) return -1;

	snprintf(mpv_sock_path, sizeof(mpv_sock_path),
	         "/tmp/nixlytile-mpv-%d.sock", (int)getpid());
	unlink(mpv_sock_path);

	mpv_media_id       = media_id;
	mpv_last_pos       = resume_pos;
	mpv_resume_target  = resume_pos;
	mpv_resume_applied = 0;
	mpv_eof_reached    = 0;

	char sock_arg[200];
	snprintf(sock_arg, sizeof(sock_arg), "--input-ipc-server=%s", mpv_sock_path);

	/* Max-performance + full mpv UI args */
	const char *argv[40];
	int ai = 0;
	argv[ai++] = "mpv";
	argv[ai++] = "--fullscreen";
	argv[ai++] = "--force-window=immediate";
	argv[ai++] = "--osc=yes";
	argv[ai++] = "--no-terminal";
	/* Verbose log level: -v equivalent (msg-level=all=v) + dump stats.
	 * Writes every decoder/demuxer/renderer event to mpv.log for
	 * post-mortem. --log-file= duplicates the terminal output to a
	 * persistent file that survives even if stdout is broken. */
	argv[ai++] = "--msg-level=all=v";
	argv[ai++] = "--log-file=/tmp/nixlytile/mpv.log";
	argv[ai++] = "--idle=no";
	argv[ai++] = "--keep-open=no";
	argv[ai++] = "--save-position-on-quit=no";
	argv[ai++] = "--pause=no";
	argv[ai++] = "--hr-seek=yes";

	/* Never let mpv self-pause waiting on cache. Initial: stream cache
	 * may fill slower than cache-pause-wait, leaving mpv stuck on the
	 * first frame until the user seeks. Runtime: brief cache underruns
	 * should stutter, not pause, to match normal player UX. */
	argv[ai++] = "--cache-pause=no";
	argv[ai++] = "--cache-pause-initial=no";
	argv[ai++] = "--cache-pause-wait=0";

	/* Video output — libplacebo path with auto API/context. */
	argv[ai++] = "--vo=gpu-next";
	argv[ai++] = "--gpu-api=auto";
	argv[ai++] = "--gpu-context=auto";
	argv[ai++] = "--hwdec=auto-safe";

	/*
	 * Clock source = AUDIO, NOT display-resample.
	 *
	 * display-resample tells mpv to resample audio to match the wl_surface
	 * frame-callback rate.  The compositor runs a fullscreen-video cadence
	 * that paces frame-done delivery to mpv; mpv's vsync estimator reads
	 * that paced rate, not the panel's real refresh rate, so the resampler
	 * calibrates against a moving target.  After a few minutes the audio
	 * speed has ramped to compensate for the mismatch and playback starts
	 * running fast.  With video-sync=audio, audio is the master clock at
	 * 1.0x and video is dropped/duplicated to match — robust, never
	 * accelerates.  profile=fast is dropped because it enables aggressive
	 * frame-drop behavior that assumed display-resample pacing.
	 */
	argv[ai++] = "--video-sync=audio";

	/* HTTP streaming cache (server delivers chunks) */
	argv[ai++] = "--cache=yes";
	argv[ai++] = "--cache-secs=60";
	argv[ai++] = "--demuxer-max-bytes=500MiB";
	argv[ai++] = "--demuxer-max-back-bytes=100MiB";
	argv[ai++] = "--demuxer-readahead-secs=20";

	/* Audio */
	argv[ai++] = "--ao=pipewire";

	/* Subtitle defaults — prefer Norwegian then English */
	argv[ai++] = "--slang=nor,nob,nno,en,eng";
	argv[ai++] = "--alang=nor,nob,nno,en,eng";

	/* Note: no --start flag. Resume seek is applied via IPC on
	 * file-loaded (see mpv_handle_event_line) because --start=N causes
	 * mpv to stall on the first frame until a manual seek. */

	argv[ai++] = sock_arg;
	argv[ai++] = url;
	argv[ai++] = NULL;

	pid_t pid = fork();
	if (pid < 0) {
		wlr_log(WLR_ERROR, "fork mpv failed: %s", strerror(errno));
		return -1;
	}
	if (pid == 0) {
		/* Child */
		setsid();
		/* Ensure log dir exists (init_logging also does this, but be
		 * defensive in case mpv is launched before full init) */
		mkdir("/tmp/nixlytile", 0755);
		/* stdin → /dev/null; stdout/stderr → stable log path. This
		 * captures EARLY output (vulkan init, missing codec, bad URL)
		 * that fires before --log-file= is opened by mpv. */
		int devnull = open("/dev/null", O_RDWR);
		if (devnull >= 0) {
			dup2(devnull, STDIN_FILENO);
			if (devnull > 2) close(devnull);
		}
		int logfd = open("/tmp/nixlytile/mpv-stdio.log",
		                 O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (logfd >= 0) {
			struct timespec ts;
			struct tm tm;
			clock_gettime(CLOCK_REALTIME, &ts);
			localtime_r(&ts.tv_sec, &tm);
			dprintf(logfd,
				"=== mpv stdio %04d-%02d-%02d %02d:%02d:%02d pid=%d ===\n",
				tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
				tm.tm_hour, tm.tm_min, tm.tm_sec, (int)getpid());
			fsync(logfd);
			dup2(logfd, STDOUT_FILENO);
			dup2(logfd, STDERR_FILENO);
			if (logfd > 2) close(logfd);
		}
		execvp("mpv", (char *const *)argv);
		_exit(127);
	}

	mpv_pid = pid;
	wlr_log(WLR_INFO, "Launched mpv pid=%d url=%s start=%.1fs",
	        pid, url, resume_pos);

	/* Wait for IPC socket, then wire it up */
	mpv_ipc_fd = mpv_ipc_connect(mpv_sock_path);
	if (mpv_ipc_fd < 0) {
		if (mpv_pid > 0) {
			kill(mpv_pid, SIGTERM);
			waitpid(mpv_pid, NULL, 0);
		}
		mpv_cleanup();
		return -1;
	}

	mpv_ipc_src = wl_event_loop_add_fd(event_loop, mpv_ipc_fd,
	                                   WL_EVENT_READABLE,
	                                   mpv_on_ipc, NULL);

	/* pidfd for exit detection */
	mpv_pidfd = pidfd_open_compat(mpv_pid);
	if (mpv_pidfd >= 0) {
		mpv_pidfd_src = wl_event_loop_add_fd(event_loop, mpv_pidfd,
		                                     WL_EVENT_READABLE,
		                                     mpv_on_pidfd, NULL);
	} else {
		wlr_log(WLR_ERROR, "pidfd_open failed (kernel <5.3?): %s — "
		                   "exit detection disabled", strerror(errno));
	}

	/* Subscribe to time-pos updates for resume saving, eof-reached for
	 * natural-end detection, and pause so we can force-resume if mpv
	 * self-pauses on first frame / cache-idle transitions. */
	mpv_launcher_send_cmd(
		"{\"command\":[\"observe_property\",1,\"time-pos\"]}");
	mpv_launcher_send_cmd(
		"{\"command\":[\"observe_property\",2,\"eof-reached\"]}");
	mpv_launcher_send_cmd(
		"{\"command\":[\"observe_property\",3,\"pause\"]}");

	/* Proactively apply the resume seek as soon as IPC is up. mpv queues
	 * seeks until the file is loaded, so this works regardless of
	 * whether file-loaded has already fired (and we may have missed
	 * that event if IPC connected after it). The file-loaded handler
	 * still runs as a fallback but won't double-seek. */
	if (mpv_resume_target > 0.5) {
		char j[128];
		snprintf(j, sizeof(j),
			"{\"command\":[\"seek\",%.3f,\"absolute\",\"exact\"]}",
			mpv_resume_target);
		mpv_launcher_send_cmd(j);
		mpv_resume_applied = 1;
	}

	/* Belt-and-suspenders: force unpause after load. The property observer
	 * handles later transitions; this covers the initial state where mpv
	 * may come up paused on first frame (some profile/cache states). */
	mpv_launcher_send_cmd(
		"{\"command\":[\"set_property\",\"pause\",false]}");

	return 0;
}

/* ── Stop ──────────────────────────────────────────────────────────── */

int
mpv_launcher_active(void)
{
	return mpv_pid > 0;
}

void
mpv_launcher_stop(void)
{
	if (mpv_pid <= 0) return;

	/* Best-effort: ask mpv to quit (so it flushes state cleanly) */
	mpv_launcher_send_cmd("{\"command\":[\"quit\"]}");

	/* Give it 300ms, else SIGTERM, else SIGKILL */
	for (int i = 0; i < 30; i++) {
		int status;
		pid_t r = waitpid(mpv_pid, &status, WNOHANG);
		if (r == mpv_pid || r < 0) goto reaped;
		struct timespec ts = { .tv_sec = 0, .tv_nsec = 10 * 1000 * 1000 };
		nanosleep(&ts, NULL);
	}
	kill(mpv_pid, SIGTERM);
	for (int i = 0; i < 30; i++) {
		int status;
		pid_t r = waitpid(mpv_pid, &status, WNOHANG);
		if (r == mpv_pid || r < 0) goto reaped;
		struct timespec ts = { .tv_sec = 0, .tv_nsec = 10 * 1000 * 1000 };
		nanosleep(&ts, NULL);
	}
	kill(mpv_pid, SIGKILL);
	waitpid(mpv_pid, NULL, 0);

reaped:
	if (mpv_media_id >= 0) {
		if (mpv_eof_reached)
			save_resume_position(mpv_media_id, 0.0);
		else if (mpv_last_pos > 1.0)
			save_resume_position(mpv_media_id, mpv_last_pos);
	}
	mpv_cleanup();
}
