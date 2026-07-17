/*
 * launchfx.c — instant Steam-launch reaction.
 *
 *  1. Detect the Play press by polling for Steam's `reaper` launch
 *     wrapper (spawned the moment Play is clicked, seconds before any
 *     window exists).
 *  2. Pre-boost: CPU governor → performance immediately, so Proton
 *     setup / shader compilation / asset loading run at full clock
 *     from t=0.  Full ultra game mode takes over when the window maps.
 *  3. Launch animation: a tiny black dot at the center of the focused
 *     monitor grows smoothly until it covers the screen, holds black
 *     through the load, and is dropped the moment the game's first
 *     window is ready — the game appears right as the black completes.
 */
#include <cairo/cairo.h>
#include <math.h>

#include "nixlytile.h"
#include "client.h"

#define FX_POLL_MS      200
#define FX_TICK_MS      16
#define FX_GROW_MS      500.0
#define FX_DOT_TEX      256   /* circle texture size; scaled up when drawn */
#define FX_WATCHDOG_MS  45000

static struct {
	int active;               /* animation running (grow or hold) */
	int grown;                /* black covers the full monitor */
	int reveal_pending;       /* game ready before grow finished */
	pid_t reaper;             /* launch wrapper that triggered us */
	Monitor *mon;
	struct wlr_scene_tree *tree;
	struct wlr_scene_buffer *dot;
	struct wlr_buffer *dot_buf;
	uint64_t start_ms;
	struct wl_event_source *tick;
	struct wl_event_source *watchdog;
} fx;

static struct wl_event_source *fx_poll_timer;
static pid_t seen_reapers[32];
static int seen_reaper_count;

/* Black anti-aliased filled circle wrapped in a PixmanBuffer (cairo
 * ARGB32 and pixman a8r8g8b8 share layout, both premultiplied). */
static struct wlr_buffer *
make_dot_buffer(int d)
{
	cairo_surface_t *cs;
	cairo_t *cr;
	struct PixmanBuffer *buf;
	void *data;
	int stride;

	cs = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, d, d);
	if (cairo_surface_status(cs) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(cs);
		return NULL;
	}
	cr = cairo_create(cs);
	cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 1.0);
	cairo_arc(cr, d / 2.0, d / 2.0, d / 2.0, 0.0,
			2.0 * 3.14159265358979323846);
	cairo_fill(cr);
	cairo_destroy(cr);
	cairo_surface_flush(cs);

	stride = cairo_image_surface_get_stride(cs);
	data = ecalloc(1, (size_t)stride * (size_t)d);
	memcpy(data, cairo_image_surface_get_data(cs),
			(size_t)stride * (size_t)d);
	cairo_surface_destroy(cs);

	buf = ecalloc(1, sizeof(*buf));
	buf->image = pixman_image_create_bits(PIXMAN_a8r8g8b8, d, d,
			data, stride);
	buf->data = data;
	buf->drm_format = DRM_FORMAT_ARGB8888;
	buf->stride = stride;
	buf->owns_data = 1;
	wlr_buffer_init(&buf->base, &pixman_buffer_impl, d, d);
	return &buf->base;
}

static void
fx_teardown(void)
{
	if (fx.tick) {
		wl_event_source_remove(fx.tick);
		fx.tick = NULL;
	}
	if (fx.watchdog) {
		wl_event_source_remove(fx.watchdog);
		fx.watchdog = NULL;
	}
	if (fx.tree) {
		wlr_scene_node_destroy(&fx.tree->node);
		fx.tree = NULL;
	}
	fx.dot = NULL;
	if (fx.dot_buf) {
		wlr_buffer_drop(fx.dot_buf);
		fx.dot_buf = NULL;
	}
	if (fx.mon && fx.mon->wlr_output)
		wlr_output_schedule_frame(fx.mon->wlr_output);
	fx.active = 0;
	fx.grown = 0;
	fx.reveal_pending = 0;
	fx.reaper = 0;
	fx.mon = NULL;
}

/* Reveal: drop the black cover — the game is ready underneath. */
static void
fx_finish(void)
{
	if (!fx.active)
		return;
	game_prelaunch_release();
	fx_teardown();
}

static void
fx_grow_complete(void)
{
	struct wlr_scene_rect *black;
	static const float black_col[4] = { 0.0f, 0.0f, 0.0f, 1.0f };

	if (!fx.mon)
		return;
	/* Crisp full-monitor rect replaces the scaled circle. */
	black = wlr_scene_rect_create(fx.tree,
			fx.mon->m.width, fx.mon->m.height, black_col);
	if (black)
		wlr_scene_node_set_position(&black->node, 0, 0);
	if (fx.dot) {
		wlr_scene_node_destroy(&fx.dot->node);
		fx.dot = NULL;
	}
	if (fx.tick) {
		wl_event_source_remove(fx.tick);
		fx.tick = NULL;
	}
	fx.grown = 1;
	if (fx.reveal_pending)
		fx_finish();
}

static int
fx_tick_cb(void *data)
{
	double t, ease, diam, final_d;
	int di;

	(void)data;
	if (!fx.active || !fx.mon || !fx.dot) {
		fx_teardown();
		return 0;
	}

	t = (double)(monotonic_msec() - fx.start_ms) / FX_GROW_MS;
	if (t > 1.0)
		t = 1.0;
	ease = t * t * (3.0 - 2.0 * t); /* smoothstep */

	final_d = ceil(hypot((double)fx.mon->m.width,
			(double)fx.mon->m.height)) + 8.0;
	diam = 8.0 + ease * (final_d - 8.0);
	di = (int)diam;

	wlr_scene_buffer_set_dest_size(fx.dot, di, di);
	wlr_scene_node_set_position(&fx.dot->node,
			(fx.mon->m.width - di) / 2,
			(fx.mon->m.height - di) / 2);
	if (fx.mon->wlr_output)
		wlr_output_schedule_frame(fx.mon->wlr_output);

	if (t >= 1.0) {
		fx_grow_complete();
		return 0;
	}
	wl_event_source_timer_update(fx.tick, FX_TICK_MS);
	return 0;
}

static int
fx_watchdog_cb(void *data)
{
	(void)data;
	/* Load took absurdly long or the game never showed a window —
	 * reveal whatever is underneath rather than staying black. */
	fx_finish();
	return 0;
}

static void
launchfx_start(pid_t reaper)
{
	if (fx.active || !selmon || !selmon->wlr_output)
		return;

	fx.mon = selmon;
	fx.dot_buf = make_dot_buffer(FX_DOT_TEX);
	if (!fx.dot_buf)
		return;
	fx.tree = wlr_scene_tree_create(layers[LyrOverlay]);
	if (!fx.tree) {
		wlr_buffer_drop(fx.dot_buf);
		fx.dot_buf = NULL;
		return;
	}
	wlr_scene_node_set_position(&fx.tree->node, fx.mon->m.x, fx.mon->m.y);
	fx.dot = wlr_scene_buffer_create(fx.tree, fx.dot_buf);
	if (!fx.dot) {
		fx_teardown();
		return;
	}

	fx.active = 1;
	fx.grown = 0;
	fx.reveal_pending = 0;
	fx.reaper = reaper;
	fx.start_ms = monotonic_msec();

	game_prelaunch_boost();

	fx.tick = wl_event_loop_add_timer(event_loop, fx_tick_cb, NULL);
	if (fx.tick)
		wl_event_source_timer_update(fx.tick, 1);
	fx.watchdog = wl_event_loop_add_timer(event_loop, fx_watchdog_cb, NULL);
	if (fx.watchdog)
		wl_event_source_timer_update(fx.watchdog, FX_WATCHDOG_MS);

	wlr_log(WLR_INFO, "launchfx: Steam launch detected (reaper pid %d) — "
			"pre-boost + cover animation", (int)reaper);
}

/* A game (or game-launcher child) window mapped — the content the user
 * is waiting for exists.  Reveal once the cover has fully grown, so the
 * sequence is always dot → full black → game. */
void
launchfx_client_mapped(Client *c)
{
	if (!fx.active || !c)
		return;
	if (!looks_like_game(c))
		return;
	if (fx.grown)
		fx_finish();
	else
		fx.reveal_pending = 1;
}

/* Belt-and-braces from rendermon: a fullscreen game is classified and
 * producing frames on some monitor. */
void
launchfx_game_ready(void)
{
	if (!fx.active)
		return;
	if (fx.grown)
		fx_finish();
	else
		fx.reveal_pending = 1;
}

static int
proc_comm_is(pid_t pid, const char *want)
{
	char path[64], comm[64];
	FILE *fp;
	char *nl;

	snprintf(path, sizeof(path), "/proc/%d/comm", (int)pid);
	fp = fopen(path, "r");
	if (!fp)
		return 0;
	comm[0] = '\0';
	if (fgets(comm, sizeof(comm), fp) && (nl = strchr(comm, '\n')))
		*nl = '\0';
	fclose(fp);
	return strcmp(comm, want) == 0;
}

static int
reaper_seen(pid_t pid)
{
	int i;
	for (i = 0; i < seen_reaper_count; i++)
		if (seen_reapers[i] == pid)
			return 1;
	return 0;
}

static int
fx_poll_cb(void *data)
{
	DIR *dir;
	struct dirent *ent;
	Client *c;
	int steam_up = 0;
	int i, j;

	(void)data;

	/* Launch aborted / game exited: the reaper wrapper lives exactly as
	 * long as the launched game.  If it died while the cover is up, the
	 * game crashed during load — reveal the desktop. */
	if (fx.active && fx.reaper > 0 && kill(fx.reaper, 0) != 0)
		fx_finish();

	/* Forget dead reapers so a later relaunch re-triggers. */
	for (i = 0; i < seen_reaper_count; ) {
		if (kill(seen_reapers[i], 0) != 0) {
			for (j = i; j < seen_reaper_count - 1; j++)
				seen_reapers[j] = seen_reapers[j + 1];
			seen_reaper_count--;
		} else {
			i++;
		}
	}

	wl_list_for_each(c, &clients, link) {
		if (is_steam_client(c)) {
			steam_up = 1;
			break;
		}
	}

	if (steam_up) {
		dir = opendir("/proc");
		if (dir) {
			while ((ent = readdir(dir))) {
				pid_t pid;
				if (ent->d_name[0] < '0' || ent->d_name[0] > '9')
					continue;
				pid = (pid_t)atoi(ent->d_name);
				if (reaper_seen(pid))
					continue;
				if (!proc_comm_is(pid, "reaper"))
					continue;
				if (seen_reaper_count <
						(int)LENGTH(seen_reapers))
					seen_reapers[seen_reaper_count++] = pid;
				launchfx_start(pid);
			}
			closedir(dir);
		}
	}

	wl_event_source_timer_update(fx_poll_timer, FX_POLL_MS);
	return 0;
}

void
launchfx_init(void)
{
	fx_poll_timer = wl_event_loop_add_timer(event_loop, fx_poll_cb, NULL);
	if (fx_poll_timer)
		wl_event_source_timer_update(fx_poll_timer, FX_POLL_MS);
}
