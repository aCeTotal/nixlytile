#include "nixlytile.h"
#include "fetch_async.h"

#include <errno.h>
#include <sys/stat.h>

#define SAMPLE_W 448
#define FALLBACK_W 1920
#define FALLBACK_H 1080
#define LUM_BINS 1024
#define LUM_HI_PCT 0.95
#define CACHE_DIR "/.cache/nixlytile"
#define STATE_DIR "/.local/state/nixly-theme"
#define SAMPLE_FILE CACHE_DIR "/wallpaper.png"
#define STAMP_FILE CACHE_DIR "/wallpaper.stamp"
#define MATUGEN_ARGS "-m dark -t scheme-tonal-spot --source-color-index 0 -q"
#define MATUGEN_OK "ok"

typedef struct {
	int out_w, out_h, strip_h;
} Geom;

typedef struct {
	char path[PATH_MAX];
	char sample[PATH_MAX];
	Geom geom;
	Backdrop backdrop;
	int saved;
} Job;

static int job_pipe[2] = { -1, -1 };
static char last_stamp[2 * PATH_MAX + 64];
static Geom last_geom;

/* Wallpaper and templates; either changing regenerates. */
static int
theme_stamp(char *out, size_t len)
{
	char wall[PATH_MAX], cfg[PATH_MAX];
	struct stat ws, cs;

	if (!realpath(wallpaper_path, wall) || stat(wall, &ws) != 0
			|| resolve_asset_path("theme/config.toml", cfg, sizeof(cfg))
			|| stat(cfg, &cs) != 0)
		return 0;
	snprintf(out, len, "%s %lld %lld %s %lld", wall, (long long)ws.st_size,
			(long long)ws.st_mtime, cfg, (long long)cs.st_mtime);
	return 1;
}

static int
home_path(const char *rel, char *out, size_t len)
{
	const char *home = getenv("HOME");

	if (!home)
		return 0;
	snprintf(out, len, "%s%s", home, rel);
	return 1;
}

/* Strip under bar, swaybg fill. */
static void
strip_box(const Job *job, int w, int h, int box[4])
{
	double s = MAX((double)job->geom.out_w / w, (double)job->geom.out_h / h);
	double vw = job->geom.out_w / s, vh = job->geom.out_h / s;

	box[0] = (int)((w - vw) / 2);
	box[1] = (int)((h - vh) / 2);
	box[2] = (int)lround(vw);
	box[3] = MAX(1, (int)lround(job->geom.strip_h / s));
}

static int
percentile(const uint32_t *hist, uint32_t total, double pct)
{
	uint32_t want = (uint32_t)(total * pct), seen = 0;

	for (int i = 0; i < LUM_BINS; i++)
		if ((seen += hist[i]) > want)
			return i;
	return LUM_BINS - 1;
}

static double
linear8(guchar v)
{
	double c = v / 255.0;

	return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

typedef struct {
	const guchar *px;
	int n, nchan;
} PixelRun;

static uint32_t
add_run(uint32_t *hist, PixelRun r)
{
	const guchar *p = r.px;

	for (int x = 0; x < r.n; x++, p += r.nchan) {
		double lum = 0.2126 * linear8(p[0]) + 0.7152 * linear8(p[1])
			+ 0.0722 * linear8(p[2]);

		hist[MIN(LUM_BINS - 1, (int)(lum * LUM_BINS))]++;
	}
	return (uint32_t)MAX(r.n, 0);
}

static void
measure(Job *job, GdkPixbuf *pb)
{
	int w = gdk_pixbuf_get_width(pb), h = gdk_pixbuf_get_height(pb);
	int nchan = gdk_pixbuf_get_n_channels(pb);
	int stride = gdk_pixbuf_get_rowstride(pb);
	const guchar *px = gdk_pixbuf_read_pixels(pb);
	uint32_t hist[LUM_BINS] = { 0 }, total = 0;
	int box[4];

	strip_box(job, w, h, box);
	for (int y = MAX(0, box[1]); y < MIN(h, box[1] + box[3]); y++)
		total += add_run(hist, (PixelRun){
			px + (size_t)y * stride + (size_t)box[0] * nchan,
			MIN(box[2], w - box[0]), nchan });
	if (!total)
		return;
	job->backdrop.y_hi = (float)(percentile(hist, total, LUM_HI_PCT) + 1) / LUM_BINS;
	job->backdrop.valid = 1;
}

static void *
analyse(void *arg)
{
	Job *job = arg;
	GdkPixbuf *raw = gdk_pixbuf_new_from_file_at_scale(job->path, SAMPLE_W, -1,
			TRUE, NULL);
	/* Upright, as swaybg shows it. */
	GdkPixbuf *pb = raw ? gdk_pixbuf_apply_embedded_orientation(raw) : NULL;

	if (raw)
		g_object_unref(raw);
	if (pb) {
		measure(job, pb);
		job->saved = gdk_pixbuf_save(pb, job->sample, "png", NULL, NULL);
		g_object_unref(pb);
	}
	(void)!write(job_pipe[1], &job, sizeof(job));
	return NULL;
}

static void
write_stamp(void)
{
	char path[PATH_MAX];
	FILE *fp;

	if (!home_path(STAMP_FILE, path, sizeof(path)) || !(fp = fopen(path, "w")))
		return;
	fputs(last_stamp, fp);
	fclose(fp);
}

static int
stamp_current(void)
{
	char path[PATH_MAX], seen[sizeof(last_stamp)] = "";
	FILE *fp;

	if (!home_path(STAMP_FILE, path, sizeof(path)) || !(fp = fopen(path, "r")))
		return 0;
	if (!fgets(seen, sizeof(seen), fp))
		seen[0] = '\0';
	fclose(fp);
	return !strcmp(seen, last_stamp);
}

/* One recolour per wallpaper, even on failure. */
static void
generated(const char *out, size_t len, void *data)
{
	(void)len;
	(void)data;
	if (!strncmp(out, MATUGEN_OK, strlen(MATUGEN_OK)))
		write_stamp();
	theme_apply();
}

/* matugen renders every template; 0 if not started. */
static int
generate(const Job *job)
{
	char cfg[PATH_MAX], cmd[3 * PATH_MAX];

	if (!job->saved || resolve_asset_path("theme/config.toml", cfg, sizeof(cfg)))
		return 0;
	snprintf(cmd, sizeof(cmd), "matugen image '%s' -c '%s' " MATUGEN_ARGS
			" && echo " MATUGEN_OK, job->sample, cfg);
	return fetch_async(cmd, generated, NULL) == 0;
}

static int
job_done(int fd, uint32_t mask, void *data)
{
	Job *job;

	(void)mask;
	(void)data;
	if (read(fd, &job, sizeof(job)) != sizeof(job))
		return 0;
	theme_set_backdrop(&job->backdrop);
	if ((!stamp_current() || !theme_apply()) && !generate(job))
		theme_apply();
	free(job);
	return 0;
}

/* mkdir -p below $HOME. */
static int
home_mkdir(const char *rel)
{
	char path[PATH_MAX];

	if (!home_path(rel, path, sizeof(path)))
		return 0;
	for (char *p = strchr(path + 1, '/'); p; p = strchr(p + 1, '/')) {
		*p = '\0';
		mkdir(path, 0700);
		*p = '/';
	}
	return mkdir(path, 0700) == 0 || errno == EEXIST;
}

/* Apps watch the state dir from login. */
static int
ensure_pipe(void)
{
	if (job_pipe[0] >= 0)
		return 1;
	if (!home_mkdir(CACHE_DIR) || !home_mkdir(STATE_DIR)
			|| pipe2(job_pipe, O_CLOEXEC) != 0)
		return 0;
	fcntl(job_pipe[0], F_SETFL, O_NONBLOCK);
	wl_event_loop_add_fd(event_loop, job_pipe[0], WL_EVENT_READABLE,
			job_done, NULL);
	return 1;
}

/* Re-measure on any change; matugen only for new art. */
void
theme_wallpaper_update(void)
{
	char stamp[sizeof(last_stamp)];
	Geom geom = {
		selmon ? selmon->m.width : FALLBACK_W,
		selmon ? selmon->m.height : FALLBACK_H,
		(int)(statusbar_top_gap + statusbar_height),
	};
	pthread_t tid;
	Job *job;

	if (!ensure_pipe() || !wallpaper_path[0] || !theme_stamp(stamp, sizeof(stamp))
			|| (!strcmp(stamp, last_stamp) && !memcmp(&geom, &last_geom, sizeof(geom))))
		return;
	snprintf(last_stamp, sizeof(last_stamp), "%s", stamp);
	last_geom = geom;
	job = ecalloc(1, sizeof(*job));
	snprintf(job->path, sizeof(job->path), "%s", wallpaper_path);
	home_path(SAMPLE_FILE, job->sample, sizeof(job->sample));
	job->geom = geom;
	if (pthread_create(&tid, NULL, analyse, job) != 0) {
		free(job);
		return;
	}
	pthread_detach(tid);
}
