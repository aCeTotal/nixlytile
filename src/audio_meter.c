/*
 * audio_meter.c — live peak levels for the volume/mic popup meters.
 *
 * While an audio popup is open, a pw-record child captures the default
 * sink monitor (volume) or default source (mic) as mono f32 @ 8 kHz and
 * the fd handler tracks the peak amplitude since the popup's last meter
 * tick.  Spawn pattern follows audio_watch.c.  Nothing runs while no
 * popup is visible.
 */
#include "nixlytile.h"

#include <fcntl.h>
#include <signal.h>

static struct wl_event_source *meter_src;
static int meter_fd = -1;
static pid_t meter_pid = -1;
static int meter_mic = -1;     /* stream type: 1 mic, 0 sink monitor */
static float meter_peak;
/* Partial float straddling a read boundary. */
static unsigned char meter_carry[4];
static size_t meter_carry_n;

void
audio_meter_stop(void)
{
	if (meter_src) {
		wl_event_source_remove(meter_src);
		meter_src = NULL;
	}
	if (meter_fd >= 0) {
		close(meter_fd);
		meter_fd = -1;
	}
	if (meter_pid > 0) {
		kill(meter_pid, SIGTERM);
		meter_pid = -1;
	}
	meter_mic = -1;
	meter_peak = 0.0f;
	meter_carry_n = 0;
}

static int
audio_meter_cb(int fd, uint32_t mask, void *data)
{
	unsigned char buf[4096];
	ssize_t n;

	(void)data;
	if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
		/* Child died (device switch, pipewire restart): the popup
		 * tick restarts the stream on its next pass. */
		audio_meter_stop();
		return 0;
	}

	while ((n = read(fd, buf + meter_carry_n,
			sizeof(buf) - meter_carry_n)) > 0) {
		size_t total = meter_carry_n + (size_t)n;
		size_t nf = total / 4;
		float s;

		if (meter_carry_n)
			memcpy(buf, meter_carry, meter_carry_n);
		for (size_t i = 0; i < nf; i++) {
			memcpy(&s, buf + i * 4, 4);
			if (s < 0.0f)
				s = -s;
			if (s > meter_peak && s <= 4.0f)
				meter_peak = s;
		}
		meter_carry_n = total - nf * 4;
		if (meter_carry_n)
			memcpy(meter_carry, buf + nf * 4, meter_carry_n);
	}
	return 0;
}

/* Ensure a capture stream of the wanted type is running. */
void
audio_meter_start(int mic)
{
	static const char *argv_mic[] = { "pw-record", "--rate", "8000",
			"--channels", "1", "--format", "f32", "-", NULL };
	static const char *argv_sink[] = { "pw-record", "-P",
			"{ stream.capture.sink = true }", "--rate", "8000",
			"--channels", "1", "--format", "f32", "-", NULL };
	pid_t pid;
	int fd;

	if (meter_fd >= 0 && meter_mic == mic)
		return;
	audio_meter_stop();

	/* posix_spawn, not fork(): reached from the popup render path */
	if (spawn_argv_read(mic ? argv_mic : argv_sink, &pid, &fd) < 0)
		return;

	meter_fd = fd;
	meter_pid = pid;
	meter_mic = mic;
	meter_peak = 0.0f;
	meter_carry_n = 0;
	meter_src = wl_event_loop_add_fd(event_loop, meter_fd,
			WL_EVENT_READABLE, audio_meter_cb, NULL);
}

int
audio_meter_running(int mic)
{
	return meter_fd >= 0 && meter_mic == mic;
}

/* Peak amplitude since the previous call; resets the accumulator. */
double
audio_meter_take_peak(void)
{
	double v = meter_peak;

	meter_peak = 0.0f;
	return v;
}
