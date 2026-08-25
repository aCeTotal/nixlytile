/*
 * presence.c — webcam presence watch → full power save.  Laptops only.
 *
 * A forked child grabs a low-res YUYV frame and prints the mean luma
 * plus an 8x6 grid of block means.  The parent compares the grid with
 * the previous sample (gain-compensated, so auto-exposure drift is not
 * motion) — a person in front of the screen always produces block-level
 * motion within a couple of minutes.  No motion AND no input for
 * PR_ABSENT_AFTER_MS → save mode: nixly-lockscreen, backlight to 0,
 * outputs off, powerprofilesctl power-saver.  The first motion sample
 * (polled every PR_INTERVAL_ABSENT_MS) or any local input brings the
 * outputs, backlight and profile straight back — the lockscreen is
 * already up, so waking lands on it.
 *
 * The luma mean doubles as the ambient sample for lightsense.c, so the
 * two never fight over the camera: while presence runs, lightsense's
 * own sampler stands down.  Visible idle-inhibitors (video players)
 * block save-entry.
 */
#include "nixlytile.h"

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>

#define PR_GRID_W 8
#define PR_GRID_H 6
#define PR_NBLOCKS (PR_GRID_W * PR_GRID_H)
#define PR_INTERVAL_PRESENT_MS 10000
#define PR_INTERVAL_ABSENT_MS  2500
#define PR_ABSENT_AFTER_MS     120000
/* Static-scene noise floor measured ≈0.1-0.6; a person's micro-motion
 * lands well above this, so 1.2 splits them with margin both ways. */
#define PR_MOTION_THRESH       1.2
#define PR_DARK_DELAY_MS       1500   /* let the lockscreen map first */

uint64_t last_key_activity_ms;

static struct wl_event_source *pr_timer;
static struct wl_event_source *pr_src;
static struct wl_event_source *pr_dark_timer;
static int pr_fd = -1;
static int pr_laptop;
static unsigned char pr_prev[PR_NBLOCKS];
static int pr_have_prev;
static uint64_t pr_last_motion_ms;
static int pr_saving;           /* 1 = lock spawned, 2 = dark applied */
static double pr_saved_backlight = -1.0;

int
presence_active(void)
{
	return pr_laptop && pr_timer != NULL;
}

/* Same one-shot V4L2 grab as lightsense, but prints mean + block grid:
 * "M <mean> <b0> ... <b47>\n". */
static void
pr_child(int wfd)
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
		unsigned long bsum[PR_NBLOCKS] = {0};
		unsigned long bcnt[PR_NBLOCKS] = {0};
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

		ns = buf.bytesused / 2;
		if (ns > (size_t)w * h)
			ns = (size_t)w * h;
		for (size_t i = 0; i < ns; i++) {
			unsigned px = (unsigned)(i % w), py = (unsigned)(i / w);
			unsigned b = (py * PR_GRID_H / h) * PR_GRID_W +
				(px * PR_GRID_W / w);
			unsigned char y = map[i * 2];

			sum += y;
			bsum[b] += y;
			bcnt[b]++;
		}
		ioctl(fd, VIDIOC_STREAMOFF, &type);
		munmap(map, buf.length);
		close(fd);
		if (ns > 0) {
			char line[512];
			int o = snprintf(line, sizeof(line), "M %lu", sum / ns);

			for (int b = 0; b < PR_NBLOCKS; b++)
				o += snprintf(line + o, sizeof(line) - o, " %lu",
						bcnt[b] ? bsum[b] / bcnt[b] : 0);
			dprintf(wfd, "%s\n", line);
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

/* A visible idle-inhibitor (video player) means someone is watching. */
static int
pr_idle_inhibited(void)
{
	struct wlr_idle_inhibitor_v1 *inhibitor;
	int lx, ly;

	wl_list_for_each(inhibitor, &idle_inhibit_mgr->inhibitors, link) {
		struct wlr_surface *surface =
			wlr_surface_get_root_surface(inhibitor->surface);
		struct wlr_scene_tree *tree = surface->data;

		if (!tree || wlr_scene_node_coords(&tree->node, &lx, &ly))
			return 1;
	}
	return 0;
}

static void
pr_outputs_set(int enabled)
{
	Monitor *m;

	wl_list_for_each(m, &mons, link) {
		struct wlr_output_state st;

		if (!m->wlr_output)
			continue;
		wlr_output_state_init(&st);
		wlr_output_state_set_enabled(&st, enabled);
		wlr_output_commit_state(m->wlr_output, &st);
		wlr_output_state_finish(&st);
	}
}

static void
pr_spawn_lock(void)
{
	pid_t pid;

	if (locked)
		return;
	pid = fork();
	if (pid == 0) {
		setsid();
		execlp("nixly-lockscreen", "nixly-lockscreen", NULL);
		_exit(127);
	}
	(void)pid;
}

/* Stage 2: lockscreen has had time to map — go fully dark. */
static int
pr_go_dark(void *data)
{
	(void)data;
	if (pr_saving != 1)
		return 0;
	pr_saving = 2;

	pr_saved_backlight = backlight_percent();
	set_backlight_percent(0.0);

	/* nobody's watching: lowest profile, lowest clock, turbo off */
	power_profile_low();
	cpuclock_boost(0);
	cpuclock_cap(0.0);

	pr_outputs_set(0);
	wlr_log(WLR_INFO, "presence: nobody in front — full power save");
	return 0;
}

static void
pr_enter_save(void)
{
	pr_saving = 1;
	pr_spawn_lock();
	if (!pr_dark_timer)
		pr_dark_timer = wl_event_loop_add_timer(event_loop, pr_go_dark,
				NULL);
	if (pr_dark_timer)
		wl_event_source_timer_update(pr_dark_timer, PR_DARK_DELAY_MS);
}

static void
pr_exit_save(void)
{
	int was_dark = pr_saving == 2;

	pr_saving = 0;
	if (pr_dark_timer)
		wl_event_source_timer_update(pr_dark_timer, 0);
	if (!was_dark)
		return;

	pr_outputs_set(1);
	/* battery → back to the battery cap, wall power → absolute best */
	powersave_reassert();
	if (light_auto_mode) {
		lightsense_sample_now();
		/* interim level until the sample lands */
		if (pr_saved_backlight > 0.0)
			set_backlight_percent(pr_saved_backlight);
	} else if (pr_saved_backlight > 0.0) {
		set_backlight_percent(pr_saved_backlight);
	}
	wlr_log(WLR_INFO, "presence: someone's back — waking to lockscreen");
}

/* Called from keypress/motion: local input is presence, wake instantly. */
void
presence_note_input(void)
{
	if (!pr_laptop)
		return;
	pr_last_motion_ms = monotonic_msec();
	if (pr_saving)
		pr_exit_save();
}

static int pr_sample(void *data);

static int
pr_read_cb(int fd, uint32_t mask, void *data)
{
	char buf[512];
	ssize_t n;

	(void)data;
	n = read(fd, buf, sizeof(buf) - 1);
	if (n > 0) {
		unsigned char grid[PR_NBLOCKS];
		int mean = 0, nb = 0;
		char *tok, *save = NULL;

		buf[n] = '\0';
		tok = strtok_r(buf, " \n", &save);
		if (tok && !strcmp(tok, "M") &&
				(tok = strtok_r(NULL, " \n", &save))) {
			mean = atoi(tok);
			while (nb < PR_NBLOCKS &&
					(tok = strtok_r(NULL, " \n", &save)))
				grid[nb++] = (unsigned char)atoi(tok);
		}

		if (nb == PR_NBLOCKS) {
			lightsense_feed_luma(mean);
			if (pr_have_prev) {
				/* gain-compensated block diff */
				int gsum = 0, psum = 0;
				double diff = 0.0;

				for (int i = 0; i < PR_NBLOCKS; i++) {
					gsum += grid[i];
					psum += pr_prev[i];
				}
				for (int i = 0; i < PR_NBLOCKS; i++)
					diff += fabs((grid[i] - gsum / (double)PR_NBLOCKS) -
						(pr_prev[i] - psum / (double)PR_NBLOCKS));
				diff /= PR_NBLOCKS;
				if (diff > PR_MOTION_THRESH) {
					pr_last_motion_ms = monotonic_msec();
					if (pr_saving)
						pr_exit_save();
				}
			}
			memcpy(pr_prev, grid, sizeof(pr_prev));
			pr_have_prev = 1;
		}
	}
	if (pr_src) {
		wl_event_source_remove(pr_src);
		pr_src = NULL;
	}
	if (pr_fd >= 0) {
		close(pr_fd);
		pr_fd = -1;
	}
	return 0;
}

static int
pr_sample(void *data)
{
	uint64_t now = monotonic_msec();
	uint64_t idle_ref;
	int fds[2];
	pid_t pid;

	(void)data;
	if (pr_timer)
		wl_event_source_timer_update(pr_timer, pr_saving ?
				PR_INTERVAL_ABSENT_MS : PR_INTERVAL_PRESENT_MS);

	/* Absence: no camera motion AND no local input for the window,
	 * nothing inhibiting idle, and not mid-game. */
	idle_ref = MAX(pr_last_motion_ms,
			MAX(last_pointer_motion_ms, last_key_activity_ms));
	if (!pr_saving && idle_ref &&
			now - idle_ref >= PR_ABSENT_AFTER_MS &&
			!pr_idle_inhibited() && !game_mode_active)
		pr_enter_save();

	if (pr_fd >= 0)   /* previous grab still in flight */
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
		pr_child(fds[1]);
		_exit(1);
	}
	close(fds[1]);
	fcntl(fds[0], F_SETFL, O_NONBLOCK);
	pr_fd = fds[0];
	pr_src = wl_event_loop_add_fd(event_loop, pr_fd,
			WL_EVENT_READABLE | WL_EVENT_HANGUP, pr_read_cb, NULL);
	return 0;
}

void
presence_init(void)
{
	struct stat st;

	/* Laptops only (backlight_available is probed lazily elsewhere, so
	 * gate on the battery — the definitive laptop signal). */
	pr_laptop = stat("/sys/class/power_supply/BAT0", &st) == 0 ||
		stat("/sys/class/power_supply/BAT1", &st) == 0;
	if (!pr_laptop)
		return;
	pr_last_motion_ms = monotonic_msec();
	if (!pr_timer)
		pr_timer = wl_event_loop_add_timer(event_loop, pr_sample, NULL);
	if (pr_timer)
		wl_event_source_timer_update(pr_timer, PR_INTERVAL_PRESENT_MS);
}
