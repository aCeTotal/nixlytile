/*
 * mic_watch.c — /dev/snd hotplug watch so the status bar's microphone
 * module appears/disappears as soon as a capture device is plugged in or
 * pulled, instead of waiting for the next 45s status refresh.
 */
#include "nixlytile.h"

#include <sys/inotify.h>

static int mic_inotify_fd = -1;
static struct wl_event_source *mic_inotify_src;

/* PipeWire needs a moment to pick up the new ALSA node before wpctl can
 * report a default source, so re-probe shortly after the device shows up. */
#define MIC_HOTPLUG_SETTLE_MS 1200

static int
mic_inotify_cb(int fd, uint32_t mask, void *data)
{
	char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
	ssize_t n;
	int changed = 0;

	(void)mask;
	(void)data;

	while ((n = read(fd, buf, sizeof(buf))) > 0) {
		char *p = buf;
		while (p < buf + n) {
			struct inotify_event *iev = (struct inotify_event *)p;
			if (iev->len > 0 && strncmp(iev->name, "pcmC", 4) == 0
					&& iev->name[strlen(iev->name) - 1] == 'c')
				changed = 1;
			p += sizeof(struct inotify_event) + iev->len;
		}
	}

	if (changed) {
		mic_last_read_ms = 0; /* skip the 8s read cache */
		set_status_task_due(refreshstatusmic,
				monotonic_msec() + MIC_HOTPLUG_SETTLE_MS);
	}
	return 0;
}

void
mic_watch_setup(void)
{
	if (mic_inotify_fd >= 0)
		return;

	mic_inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (mic_inotify_fd < 0)
		return;
	if (inotify_add_watch(mic_inotify_fd, "/dev/snd",
				IN_CREATE | IN_DELETE) < 0) {
		close(mic_inotify_fd);
		mic_inotify_fd = -1;
		return;
	}
	mic_inotify_src = wl_event_loop_add_fd(event_loop, mic_inotify_fd,
			WL_EVENT_READABLE, mic_inotify_cb, NULL);
}

void
mic_watch_cleanup(void)
{
	if (mic_inotify_src) {
		wl_event_source_remove(mic_inotify_src);
		mic_inotify_src = NULL;
	}
	if (mic_inotify_fd >= 0) {
		close(mic_inotify_fd);
		mic_inotify_fd = -1;
	}
}
