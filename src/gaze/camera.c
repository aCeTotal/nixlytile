#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "camera.h"

#define VIDEO_NODES 16
#define MIN_FPS 30
#define MAX_FPS 60

typedef struct {
	uint32_t format;
	int w, h;
} Mode;

/* Bigger frames first; skips IR. */
static const Mode modes[] = {
	{ V4L2_PIX_FMT_MJPEG, 1920, 1080 },
	{ V4L2_PIX_FMT_MJPEG, 1280, 720 },
	{ V4L2_PIX_FMT_YUYV, 1280, 720 },
	{ V4L2_PIX_FMT_YUYV, 640, 480 },
};

static int
capture_node(int fd)
{
	struct v4l2_capability cap;
	uint32_t caps;

	if (ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0)
		return 0;
	caps = cap.capabilities & V4L2_CAP_DEVICE_CAPS ? cap.device_caps : cap.capabilities;
	return (caps & V4L2_CAP_VIDEO_CAPTURE) && (caps & V4L2_CAP_STREAMING);
}

/* Mode and its frame rate. */
typedef struct {
	size_t mode;
	unsigned fps;
} Choice;

/* Capped rate, 0 when unusable. */
static unsigned
rate(int fd, const Mode *m)
{
	struct v4l2_frmivalenum iv = { .pixel_format = m->format, .width = (uint32_t)m->w,
		.height = (uint32_t)m->h };
	unsigned best = 0;

	for (; ioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &iv) == 0; iv.index++) {
		const struct v4l2_fract *t = iv.type == V4L2_FRMIVAL_TYPE_DISCRETE ?
			&iv.discrete : &iv.stepwise.min;
		unsigned fps = t->numerator ? t->denominator / t->numerator : 0;

		best = fps > best ? fps : best;
	}
	best = best > MAX_FPS ? MAX_FPS : best;
	return best >= MIN_FPS ? best : 0;
}

/* Higher rate, then earlier mode. */
static int
better(Choice a, Choice b)
{
	return a.fps > b.fps || (a.fps == b.fps && a.mode < b.mode);
}

/* Best mode, or fps 0. */
static Choice
best_mode(int fd)
{
	Choice best = { 0, 0 };

	if (!capture_node(fd))
		return best;
	for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
		Choice c = { i, rate(fd, &modes[i]) };

		if (c.fps && better(c, best))
			best = c;
	}
	return best;
}

/* Keeps the better node open. */
static void
consider(Camera *cam, int n, Choice *best)
{
	char path[sizeof(cam->path)];
	int fd;
	Choice m;

	snprintf(path, sizeof(path), "/dev/video%d", n);
	fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		return;
	m = best_mode(fd);
	if (!m.fps || !better(m, *best)) {
		close(fd);
		return;
	}
	if (cam->fd >= 0)
		close(cam->fd);
	cam->fd = fd;
	*best = m;
	memcpy(cam->path, path, sizeof(path));
}

static Choice
pick_node(Camera *cam)
{
	Choice best = { 0, 0 };

	cam->fd = -1;
	for (int n = 0; n < VIDEO_NODES; n++)
		consider(cam, n, &best);
	return best;
}

static int
set_mode(Camera *cam, const Mode *m, unsigned fps)
{
	struct v4l2_format fmt = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
	struct v4l2_streamparm parm = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };

	fmt.fmt.pix = (struct v4l2_pix_format){ .width = (uint32_t)m->w, .height = (uint32_t)m->h,
		.pixelformat = m->format, .field = V4L2_FIELD_NONE };
	if (ioctl(cam->fd, VIDIOC_S_FMT, &fmt) < 0 || fmt.fmt.pix.pixelformat != m->format ||
	    fmt.fmt.pix.width != (uint32_t)m->w || fmt.fmt.pix.height != (uint32_t)m->h)
		return -1;
	parm.parm.capture.timeperframe = (struct v4l2_fract){ 1, fps };
	cam->fps = ioctl(cam->fd, VIDIOC_S_PARM, &parm) == 0 && parm.parm.capture.timeperframe.numerator ?
		(int)(parm.parm.capture.timeperframe.denominator / parm.parm.capture.timeperframe.numerator) :
		(int)fps;
	cam->w = m->w;
	cam->h = m->h;
	cam->jpeg = m->format == V4L2_PIX_FMT_MJPEG;
	return 0;
}

/* Darkness would halve the rate. */
static void
hold_frame_rate(Camera *cam)
{
	struct v4l2_control ctl = { .id = V4L2_CID_EXPOSURE_AUTO_PRIORITY };

	cam->priority = ioctl(cam->fd, VIDIOC_G_CTRL, &ctl) == 0 ? ctl.value : -1;
	if (cam->priority <= 0)
		return;
	ctl.value = 0;
	ioctl(cam->fd, VIDIOC_S_CTRL, &ctl);
}

static int
queue(const Camera *cam, int i)
{
	struct v4l2_buffer buf = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP,
		.index = (uint32_t)i };

	return ioctl(cam->fd, VIDIOC_QBUF, &buf);
}

static int
map_buffer(Camera *cam, int i)
{
	struct v4l2_buffer buf = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP,
		.index = (uint32_t)i };

	if (ioctl(cam->fd, VIDIOC_QUERYBUF, &buf) < 0)
		return -1;
	cam->map[i] = mmap(NULL, buf.length, PROT_READ, MAP_SHARED, cam->fd, buf.m.offset);
	if (cam->map[i] == MAP_FAILED) {
		cam->map[i] = NULL;
		return -1;
	}
	cam->maplen[i] = buf.length;
	return queue(cam, i);
}

static int
start(Camera *cam)
{
	struct v4l2_requestbuffers req = { .count = CAMERA_BUFFERS,
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP };
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

	if (ioctl(cam->fd, VIDIOC_REQBUFS, &req) < 0 || req.count < 2)
		return -1;
	cam->nbuf = req.count < CAMERA_BUFFERS ? (int)req.count : CAMERA_BUFFERS;
	for (int i = 0; i < cam->nbuf; i++)
		if (map_buffer(cam, i) < 0)
			return -1;
	return ioctl(cam->fd, VIDIOC_STREAMON, &type);
}

int
camera_open(Camera *cam)
{
	Choice c;

	memset(cam, 0, sizeof(*cam));
	cam->held = -1;
	cam->priority = -1;
	c = pick_node(cam);
	if (cam->fd < 0)
		return -1;
	if (set_mode(cam, &modes[c.mode], c.fps) == 0) {
		hold_frame_rate(cam);
		if (start(cam) == 0)
			return 0;
	}
	camera_close(cam);
	return -1;
}

void
camera_close(Camera *cam)
{
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	struct v4l2_control ctl = { .id = V4L2_CID_EXPOSURE_AUTO_PRIORITY, .value = cam->priority };

	if (cam->fd < 0)
		return;
	ioctl(cam->fd, VIDIOC_STREAMOFF, &type);
	for (int i = 0; i < cam->nbuf; i++)
		if (cam->map[i])
			munmap(cam->map[i], cam->maplen[i]);
	if (cam->priority > 0)
		ioctl(cam->fd, VIDIOC_S_CTRL, &ctl);
	close(cam->fd);
	cam->fd = -1;
}

/* Capture time, compositor clock. */
static double
stamp(const struct v4l2_buffer *b)
{
	struct timespec now;

	if ((b->flags & V4L2_BUF_FLAG_TIMESTAMP_MASK) == V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC)
		return (double)b->timestamp.tv_sec + (double)b->timestamp.tv_usec / 1e6;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

/* Newest frame; older ones requeue. */
CameraStatus
camera_next(Camera *cam, Frame *f, int timeout_ms)
{
	struct pollfd p = { cam->fd, POLLIN, 0 };
	struct v4l2_buffer buf = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP };
	struct v4l2_buffer newest = buf;
	int n;

	if (cam->held >= 0)
		queue(cam, cam->held);
	cam->held = -1;
	n = poll(&p, 1, timeout_ms);
	if ((n < 0 && errno != EINTR) || p.revents & (POLLERR | POLLHUP | POLLNVAL))
		return CAMERA_GONE;
	if (n <= 0)
		return CAMERA_WAIT;
	while (ioctl(cam->fd, VIDIOC_DQBUF, &buf) == 0) {
		if (cam->held >= 0)
			queue(cam, cam->held);
		cam->held = (int)buf.index;
		newest = buf;
	}
	if (errno == ENODEV)
		return CAMERA_GONE;
	if (cam->held < 0 || newest.flags & V4L2_BUF_FLAG_ERROR || !newest.bytesused)
		return CAMERA_WAIT;
	*f = (Frame){ cam->map[cam->held], newest.bytesused, cam->w, cam->h, cam->jpeg, stamp(&newest) };
	return CAMERA_FRAME;
}
