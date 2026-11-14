#include "nixlytile.h"

#define THUMB_SRC_MAX 4096
#define THUMB_MAX_W   340
#define THUMB_MAX_H   220

typedef struct {
	char path[PATH_MAX];
	thumb_done_fn done;
	void *data;
	cairo_surface_t *img;
} ThumbJob;

static int thumb_pipe[2] = { -1, -1 };

static cairo_surface_t *
thumb_surface(GdkPixbuf *pb)
{
	int w = gdk_pixbuf_get_width(pb);
	int h = gdk_pixbuf_get_height(pb);
	int nchan = gdk_pixbuf_get_n_channels(pb);
	int alpha = gdk_pixbuf_get_has_alpha(pb);
	int sstride = gdk_pixbuf_get_rowstride(pb);
	const guchar *src = gdk_pixbuf_read_pixels(pb);
	cairo_surface_t *cs;
	uint8_t *dst;
	int dstride;

	cs = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
	if (cairo_surface_status(cs) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(cs);
		return NULL;
	}
	cairo_surface_flush(cs);
	dst = cairo_image_surface_get_data(cs);
	dstride = cairo_image_surface_get_stride(cs);
	for (int y = 0; y < h; y++) {
		const guchar *p = src + (size_t)y * (size_t)sstride;
		uint32_t *q = (uint32_t *)(dst + (size_t)y * (size_t)dstride);

		for (int x = 0; x < w; x++, p += nchan) {
			uint32_t a = alpha ? p[3] : 255;

			q[x] = a << 24 | (p[0] * a / 255) << 16 |
				(p[1] * a / 255) << 8 | p[2] * a / 255;
		}
	}
	cairo_surface_mark_dirty(cs);
	return cs;
}

static cairo_surface_t *
thumb_load(const char *path)
{
	GdkPixbuf *pb;
	cairo_surface_t *cs;
	int fw, fh;

	if (!gdk_pixbuf_get_file_info(path, &fw, &fh) ||
			fw > THUMB_SRC_MAX || fh > THUMB_SRC_MAX)
		return NULL;
	pb = gdk_pixbuf_new_from_file_at_scale(path, MIN(fw, THUMB_MAX_W),
			MIN(fh, THUMB_MAX_H), TRUE, NULL);
	if (!pb)
		return NULL;
	cs = gdk_pixbuf_get_n_channels(pb) >= 3 ? thumb_surface(pb) : NULL;
	g_object_unref(pb);
	return cs;
}

static int
thumb_ready(int fd, uint32_t mask, void *unused)
{
	ThumbJob *j;

	while (read(fd, &j, sizeof(j)) == sizeof(j)) {
		j->done(j->img, j->data);
		free(j);
	}
	return 0;
}

static void *
thumb_worker(void *arg)
{
	ThumbJob *j = arg;

	j->img = thumb_load(j->path);
	if (write(thumb_pipe[1], &j, sizeof(j)) != sizeof(j))
		nx_log(NX_ERROR, "thumb: lost result for %s", j->path);
	return NULL;
}

static int
thumb_init(void)
{
	if (thumb_pipe[0] >= 0)
		return 0;
	if (pipe2(thumb_pipe, O_CLOEXEC) < 0)
		return -1;
	fcntl(thumb_pipe[0], F_SETFL, O_NONBLOCK);
	wl_event_loop_add_fd(event_loop, thumb_pipe[0], WL_EVENT_READABLE,
			thumb_ready, NULL);
	return 0;
}

/* Done runs on event loop. */
void
thumb_load_async(const char *path, thumb_done_fn done, void *data)
{
	ThumbJob *j = calloc(1, sizeof(*j));
	pthread_attr_t attr;
	pthread_t tid;
	int r;

	if (!j || thumb_init() < 0) {
		free(j);
		done(NULL, data);
		return;
	}
	snprintf(j->path, sizeof(j->path), "%s", path);
	j->done = done;
	j->data = data;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	r = pthread_create(&tid, &attr, thumb_worker, j);
	pthread_attr_destroy(&attr);
	if (r == 0)
		return;
	free(j);
	done(NULL, data);
}
