/*
 * camwatch.c — every webcam grab happens on this thread, never on the
 * compositor thread.
 *
 * Why: a V4L2 grab blocks for hundreds of ms (STREAMON plus waiting for
 * the first frame), so presence.c and lightsense.c each used to fork()
 * a child to do it off-loop.  But fork() itself is the stall — copying
 * the compositor's page tables freezes the cursor, and presence forks
 * every 2.5s while it thinks nobody is there.  Here one worker thread
 * owns /dev/video*, publishes a snapshot under a mutex and pokes the
 * event loop through a pipe; the compositor thread only ever asks for a
 * grab and copies the result.
 *
 * Grabs are request-driven — presence.c and lightsense.c keep their own
 * cadence timers, and the thread sleeps on a condvar in between, so the
 * camera (and its LED) stays untouched unless someone asks.
 */
#include "nixlytile.h"

#include <fcntl.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>

static pthread_t cw_thread;
static pthread_mutex_t cw_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cw_cond = PTHREAD_COND_INITIALIZER;
static CamSnapshot cw_state;           /* guarded by cw_lock */
static int cw_want;                    /* guarded by cw_lock */
static int cw_run;                     /* guarded by cw_lock */
static int cw_pipe[2] = { -1, -1 };
static struct wl_event_source *cw_src;

/* One-shot grab: first /dev/video* that yields a YUYV frame wins.  Fills
 * mean luma and the CAM_GRID_W x CAM_GRID_H grid of block means. */
static int
cw_grab(CamSnapshot *s)
{
	static const char *devs[] = {
		"/dev/video0", "/dev/video1", "/dev/video2", "/dev/video3",
	};

	for (size_t d = 0; d < LENGTH(devs); d++) {
		struct v4l2_capability cap;
		struct v4l2_format fmt = {0};
		struct v4l2_requestbuffers req = {0};
		struct v4l2_buffer buf = {0};
		enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		unsigned char *map = MAP_FAILED;
		fd_set fds;
		struct timeval tv = { 3, 0 };
		unsigned long bsum[CAM_NBLOCKS] = {0};
		unsigned long bcnt[CAM_NBLOCKS] = {0};
		unsigned long sum = 0;
		unsigned w, h;
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
		w = fmt.fmt.pix.width;
		h = fmt.fmt.pix.height;
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
		if (ns > (size_t)w * h)
			ns = (size_t)w * h;
		for (size_t i = 0; i < ns; i++) {
			unsigned px = (unsigned)(i % w), py = (unsigned)(i / w);
			unsigned b = (py * CAM_GRID_H / h) * CAM_GRID_W +
				(px * CAM_GRID_W / w);
			unsigned char y = map[i * 2];

			sum += y;
			bsum[b] += y;
			bcnt[b]++;
		}
		ioctl(fd, VIDIOC_STREAMOFF, &type);
		munmap(map, buf.length);
		close(fd);
		if (ns == 0)
			continue;
		s->mean = (int)(sum / ns);
		for (int b = 0; b < CAM_NBLOCKS; b++)
			s->grid[b] = (unsigned char)(bcnt[b] ?
					bsum[b] / bcnt[b] : 0);
		return 1;
next:
		if (map != MAP_FAILED)
			munmap(map, buf.length);
		close(fd);
	}
	return 0;
}

static void *
cw_worker(void *data)
{
	(void)data;
	pthread_setname_np(pthread_self(), "nixly-camwatch");

	for (;;) {
		CamSnapshot local = {0};

		pthread_mutex_lock(&cw_lock);
		while (cw_run && !cw_want)
			pthread_cond_wait(&cw_cond, &cw_lock);
		if (!cw_run) {
			pthread_mutex_unlock(&cw_lock);
			break;
		}
		cw_want = 0;
		pthread_mutex_unlock(&cw_lock);

		/* camera I/O outside the lock: a grab can block seconds */
		if (!cw_grab(&local))
			continue;
		local.valid = 1;
		local.stamp_ms = monotonic_msec();

		pthread_mutex_lock(&cw_lock);
		cw_state = local;
		pthread_mutex_unlock(&cw_lock);

		if (cw_pipe[1] >= 0)
			(void)!write(cw_pipe[1], "c", 1);
	}
	return NULL;
}

static int
cw_event(int fd, uint32_t mask, void *data)
{
	CamSnapshot s;
	char buf[16];

	(void)mask;
	(void)data;
	while (read(fd, buf, sizeof(buf)) > 0)
		;
	if (!camwatch_get(&s) || !s.valid)
		return 0;
	presence_camera_frame(&s);
	lightsense_camera_frame(&s);
	return 0;
}

int
camwatch_get(CamSnapshot *out)
{
	pthread_mutex_lock(&cw_lock);
	*out = cw_state;
	pthread_mutex_unlock(&cw_lock);
	return out->stamp_ms != 0;
}

/* Ask for one grab.  Coalesces: a request arriving while a grab is in
 * flight is dropped, the in-flight frame serves both callers. */
void
camwatch_request(void)
{
	pthread_mutex_lock(&cw_lock);
	if (cw_run) {
		cw_want = 1;
		pthread_cond_signal(&cw_cond);
	}
	pthread_mutex_unlock(&cw_lock);
}

void
camwatch_init(void)
{
	if (pipe2(cw_pipe, O_CLOEXEC | O_NONBLOCK) < 0)
		return;
	cw_src = wl_event_loop_add_fd(event_loop, cw_pipe[0],
			WL_EVENT_READABLE, cw_event, NULL);
	cw_run = 1;
	if (pthread_create(&cw_thread, NULL, cw_worker, NULL) != 0) {
		cw_run = 0;
		wlr_log(WLR_ERROR, "camwatch: thread start failed — "
			"presence and auto-brightness unavailable");
	}
}
