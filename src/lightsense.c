/*
 * lightsense.c — webcam-based auto brightness for the light module.
 *
 * Two modes.  Auto (default at every login): a forked child grabs one
 * low-res frame from the first working /dev/video* every LS_INTERVAL_MS,
 * the mean luma maps to a backlight target (dark room → low, normal
 * room → ~40%, bright room → high) and is applied with hysteresis.
 * Manual: any hand adjustment (scroll, slider drag, popup button) locks
 * the level for the rest of the session.  The manual value is saved in
 * ~/.local/nixlyos/ and remembered across logins, but the mode itself
 * always starts as Auto.
 *
 * The capture runs entirely in the child (V4L2 dequeue can block for
 * hundreds of ms); the parent just reads "<luma>\n" from a pipe.
 */
#include "nixlytile.h"

#include <fcntl.h>
#include <pwd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>

#define LS_INTERVAL_MS 30000
#define LS_FIRST_MS    2500
#define LS_HYSTERESIS  5.0

int light_auto_mode = 1;
double light_manual_value = -1.0;
int light_ambient_luma = -1;

static struct wl_event_source *ls_timer;
static struct wl_event_source *ls_src;
static int ls_fd = -1;
static char ls_conf_path[PATH_MAX];

static void
ls_resolve_path(void)
{
	const char *home = getenv("HOME");

	if (!home) {
		struct passwd *pw = getpwuid(getuid());
		if (pw)
			home = pw->pw_dir;
	}
	if (!home)
		home = "/";
	snprintf(ls_conf_path, sizeof(ls_conf_path),
			"%s/.local/nixlyos/brightness.conf", home);
}

static void
ls_save(void)
{
	FILE *fp;
	char dir[PATH_MAX];
	char *slash;

	if (!ls_conf_path[0])
		return;
	snprintf(dir, sizeof(dir), "%s", ls_conf_path);
	slash = strrchr(dir, '/');
	if (slash) {
		*slash = '\0';
		mkdir(dir, 0755);
	}
	fp = fopen(ls_conf_path, "w");
	if (!fp)
		return;
	fprintf(fp, "manual %.1f\n", light_manual_value);
	fclose(fp);
}

static void
ls_load(void)
{
	FILE *fp = fopen(ls_conf_path, "r");
	double v;

	if (!fp)
		return;
	if (fscanf(fp, "manual %lf", &v) == 1 && v >= 0.0 && v <= 100.0)
		light_manual_value = v;
	fclose(fp);
}

/* One-shot V4L2 grab in the child: first camera that yields a YUYV
 * frame wins; prints the mean luma (0-255) and exits. */
static void
ls_child(int wfd)
{
	static const char *devs[] = {
		"/dev/video0", "/dev/video1", "/dev/video2", "/dev/video3",
	};

	alarm(5);
	for (size_t d = 0; d < LENGTH(devs); d++) {
		struct v4l2_capability cap;
		struct v4l2_format fmt = {0};
		struct v4l2_requestbuffers req = {0};
		struct v4l2_buffer buf = {0};
		enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		unsigned char *map = MAP_FAILED;
		fd_set fds;
		struct timeval tv = { 3, 0 };
		unsigned long sum = 0;
		size_t ns;
		int fd = open(devs[d], O_RDWR | O_CLOEXEC);

		if (fd < 0)
			continue;
		if (ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0 ||
				!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) ||
				!(cap.capabilities & V4L2_CAP_STREAMING))
			goto next;

		fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		fmt.fmt.pix.width = 160;
		fmt.fmt.pix.height = 120;
		fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
		fmt.fmt.pix.field = V4L2_FIELD_ANY;
		if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0 ||
				fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_YUYV)
			goto next;

		req.count = 1;
		req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		req.memory = V4L2_MEMORY_MMAP;
		if (ioctl(fd, VIDIOC_REQBUFS, &req) < 0 || req.count < 1)
			goto next;
		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index = 0;
		if (ioctl(fd, VIDIOC_QUERYBUF, &buf) < 0)
			goto next;
		map = mmap(NULL, buf.length, PROT_READ, MAP_SHARED, fd,
				buf.m.offset);
		if (map == MAP_FAILED)
			goto next;
		if (ioctl(fd, VIDIOC_QBUF, &buf) < 0 ||
				ioctl(fd, VIDIOC_STREAMON, &type) < 0)
			goto next;

		FD_ZERO(&fds);
		FD_SET(fd, &fds);
		if (select(fd + 1, &fds, NULL, NULL, &tv) <= 0 ||
				ioctl(fd, VIDIOC_DQBUF, &buf) < 0) {
			ioctl(fd, VIDIOC_STREAMOFF, &type);
			goto next;
		}

		/* YUYV: every even byte is a Y sample */
		ns = buf.bytesused / 2;
		for (size_t i = 0; i < ns; i++)
			sum += map[i * 2];
		ioctl(fd, VIDIOC_STREAMOFF, &type);
		munmap(map, buf.length);
		close(fd);
		if (ns > 0) {
			dprintf(wfd, "%lu\n", sum / ns);
			_exit(0);
		}
		continue;
next:
		if (map != MAP_FAILED)
			munmap(map, buf.length);
		close(fd);
	}
	_exit(1);
}

/* Mean luma → backlight percent, biased as low as comfortably readable:
 * pitch dark ≈8, dim ≈8-25, normal indoor ≈25-38, bright room climbs to
 * 60, direct daylight caps at 80. */
static double
ls_target(int luma)
{
	if (luma <= 25)
		return 8.0;
	if (luma <= 85)
		return 8.0 + (luma - 25) * (25.0 - 8.0) / 60.0;
	if (luma <= 150)
		return 25.0 + (luma - 85) * (38.0 - 25.0) / 65.0;
	if (luma >= 210)
		return 80.0;
	return 38.0 + (luma - 150) * (80.0 - 38.0) / 60.0;
}

static void
ls_apply(int luma)
{
	double target, cur;

	light_ambient_luma = luma;
	if (!light_auto_mode || !backlight_available)
		return;
	target = ls_target(luma);
	cur = backlight_percent();
	if (cur >= 0.0 && fabs(target - cur) < LS_HYSTERESIS)
		return;
	if (set_backlight_percent(target) != 0)
		return;
	light_last_percent = target;
	light_cached_percent = target;
	refreshstatuslight();
}

static int
ls_read_cb(int fd, uint32_t mask, void *data)
{
	char buf[32];
	ssize_t n;

	(void)data;
	n = read(fd, buf, sizeof(buf) - 1);
	if (n > 0) {
		buf[n] = '\0';
		ls_apply(atoi(buf));
	}
	if (ls_src) {
		wl_event_source_remove(ls_src);
		ls_src = NULL;
	}
	if (ls_fd >= 0) {
		close(ls_fd);
		ls_fd = -1;
	}
	return 0;
}

/* presence.c samples the same camera and forwards its mean luma here —
 * don't fight over the device while it runs. */
void
lightsense_feed_luma(int luma)
{
	ls_apply(luma);
}

static int
ls_sample(void *data)
{
	int fds[2];
	pid_t pid;

	(void)data;
	if (ls_timer)
		wl_event_source_timer_update(ls_timer, LS_INTERVAL_MS);
	if (ls_fd >= 0)   /* previous grab still in flight */
		return 0;
	if (!light_auto_mode || presence_active())
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
		close(fds[0]);
		setsid();
		ls_child(fds[1]);
		_exit(1);
	}
	close(fds[1]);
	fcntl(fds[0], F_SETFL, O_NONBLOCK);
	ls_fd = fds[0];
	ls_src = wl_event_loop_add_fd(event_loop, ls_fd,
			WL_EVENT_READABLE | WL_EVENT_HANGUP, ls_read_cb, NULL);
	return 0;
}

void
lightsense_sample_now(void)
{
	/* presence.c owns the camera on laptops — route through it so the
	 * grab happens even during input-activity (single LED blip) */
	if (presence_active()) {
		presence_sample_once();
		return;
	}
	if (ls_timer)
		wl_event_source_timer_update(ls_timer, 1);
}

/* Hand adjustment: lock the level for this session and remember it. */
void
light_mode_set_manual(double value)
{
	light_auto_mode = 0;
	if (value >= 0.0 && value <= 100.0)
		light_manual_value = value;
	ls_save();
}

void
light_mode_set_auto(void)
{
	light_auto_mode = 1;
	lightsense_sample_now();
}

void
lightsense_init(void)
{
	ls_resolve_path();
	ls_load();
	if (!ls_timer)
		ls_timer = wl_event_loop_add_timer(event_loop, ls_sample, NULL);
	if (ls_timer)
		wl_event_source_timer_update(ls_timer, LS_FIRST_MS);
}
