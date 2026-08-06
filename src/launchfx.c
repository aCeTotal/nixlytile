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
#include <fcntl.h>
#include <math.h>
#include <unistd.h>

#include "nixlytile.h"
#include "client.h"

#define FX_POLL_MS      200
#define FX_TICK_MS      16
#define FX_GROW_MS      500.0
#define FX_HEADSTART_MS 1000  /* cover plays this long before ultra mode */
#define FX_DOT_TEX      256   /* circle texture size; scaled up when drawn */
#define FX_WATCHDOG_MS  45000
/* One Play press spawns a CHAIN of short-lived reaper processes
 * (first-time setup, install scripts) before the game's own reaper.
 * When the tracked reaper dies, wait this long for the next stage
 * before concluding the launch was aborted. */
#define FX_CHAIN_GRACE_MS 3000
/* A game window mapped but never went fullscreen: it's an interactive
 * launcher/config dialog the user must click — reveal it. */
#define FX_LAUNCHER_MS  10000

static struct {
	int active;               /* animation running (grow or hold) */
	int grown;                /* black covers the full monitor */
	int reveal_pending;       /* game ready before grow finished */
	int content_seen;         /* fullscreen game committed a buffer under the cover */
	pid_t reaper;             /* newest live reaper of the launch chain */
	uint64_t orphan_ms;       /* when the tracked reaper died; 0 = alive */
	uint64_t window_ms;       /* when the first game window mapped; 0 = none */
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
	fx.content_seen = 0;
	fx.reaper = 0;
	fx.orphan_ms = 0;
	fx.window_ms = 0;
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
	if (fx.reveal_pending && fx.content_seen)
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

/* Warm the page cache for the game being launched: reaper's cmdline
 * carries "AppId=N".  Fire-and-forget; nixly-prewarm does the work at
 * idle I/O priority. */
static void
fx_readahead(pid_t reaper)
{
	char path[64], buf[4096], appid[32];
	ssize_t n;
	int fd, i;
	char *p = NULL;

	snprintf(path, sizeof(path), "/proc/%d/cmdline", (int)reaper);
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return;
	buf[n] = '\0';
	/* cmdline args are NUL-separated — scan every arg */
	for (i = 0; i < (int)n; i += (int)strlen(buf + i) + 1) {
		if (strncmp(buf + i, "AppId=", 6) == 0) {
			p = buf + i + 6;
			break;
		}
	}
	if (!p || !p[0])
		return;
	snprintf(appid, sizeof(appid), "%s", p);

	if (fork() == 0) {
		setsid();
		execlp("nixly-prewarm", "nixly-prewarm", "readahead", appid,
				(char *)NULL);
		_exit(127);
	}
}

/* Start tracking a launch: pre-boost + chain/abort watchdog.  No visual
 * — the cover animation plays later, at game-mode activation. */
static void
fx_track_start(void)
{
	fx.active = 1;
	fx.grown = 0;
	fx.reveal_pending = 0;
	fx.content_seen = 0;
	fx.reaper = 0;
	fx.orphan_ms = 0;
	fx.window_ms = 0;

	game_prelaunch_boost();

	fx.watchdog = wl_event_loop_add_timer(event_loop, fx_watchdog_cb, NULL);
	if (fx.watchdog)
		wl_event_source_timer_update(fx.watchdog, FX_WATCHDOG_MS);
}

/* Build the cover scene on m and start the grow tick.  Returns 0 on
 * allocation failure. */
static int
fx_start_cover(Monitor *m)
{
	fx.mon = m;
	fx.dot_buf = make_dot_buffer(FX_DOT_TEX);
	if (!fx.dot_buf)
		return 0;
	fx.tree = wlr_scene_tree_create(layers[LyrOverlay]);
	if (!fx.tree) {
		wlr_buffer_drop(fx.dot_buf);
		fx.dot_buf = NULL;
		return 0;
	}
	wlr_scene_node_set_position(&fx.tree->node, fx.mon->m.x, fx.mon->m.y);
	fx.dot = wlr_scene_buffer_create(fx.tree, fx.dot_buf);
	if (!fx.dot) {
		fx_teardown();
		return 0;
	}

	fx.start_ms = monotonic_msec();

	fx.tick = wl_event_loop_add_timer(event_loop, fx_tick_cb, NULL);
	if (fx.tick)
		wl_event_source_timer_update(fx.tick, 1);
	return 1;
}

static void
launchfx_start(pid_t reaper)
{
	Client *fsc;

	if (fx.active || !selmon || !selmon->wlr_output)
		return;

	/* A fullscreen game is already on screen (e.g. Steam runs a setup
	 * step for something else) — game mode owns the tuning already. */
	fsc = fullscreen_visible_on(selmon);
	if (fsc && looks_like_game(fsc))
		return;

	fx_readahead(reaper);

	fx_track_start();
	fx.reaper = reaper;

	wlr_log(WLR_INFO, "launchfx: Steam launch detected (reaper pid %d) — "
			"pre-boost; cover plays at game-mode activation", (int)reaper);
}

/* Called from update_game_mode() the moment ultra game mode activates for
 * a fullscreen game: play the cover grow and let rendermon's
 * launchfx_game_ready() reveal the presenting game. */
void
launchfx_fullscreen_starting(Client *c)
{
	if (!c || !c->mon || !c->mon->wlr_output)
		return;
	if (fx.tree || c->fx_covered || !looks_like_game(c))
		return;
	c->fx_covered = 1;
	if (!fx.active)
		fx_track_start();
	if (!fx_start_cover(c->mon))
		return;
	/* Arm the interactive-launcher fallback: the window exists, so a
	 * game that fullscreens but never presents must still be revealed. */
	if (!fx.window_ms)
		fx.window_ms = monotonic_msec();

	wlr_log(WLR_INFO, "launchfx: game mode activated — "
			"playing cover animation on %s", c->mon->wlr_output->name);
}

/* Head start for the cover: update_game_mode() delays ultra activation
 * until the cover has been playing this long, so the grow animation
 * finishes before the game (direct scanout) takes over the screen.
 * Returns remaining ms, 0 when no cover is up or the time has passed. */
int
launchfx_cover_headstart_remaining(void)
{
	uint64_t elapsed;

	if (!fx.tree)
		return 0;
	elapsed = monotonic_msec() - fx.start_ms;
	if (elapsed >= FX_HEADSTART_MS)
		return 0;
	return (int)(FX_HEADSTART_MS - elapsed);
}

/* A game (or game-launcher child) window mapped.  Do NOT reveal yet —
 * Proton titles map splash/launcher windows first.  The cover stays up
 * until the game is fullscreen and presenting (launchfx_game_ready);
 * this timestamp only arms the interactive-launcher fallback in the
 * poll timer. */
void
launchfx_client_mapped(Client *c)
{
	if (!fx.active || !c)
		return;
	if (!looks_like_game(c))
		return;
	/* Unmanaged (override-redirect) windows are splash screens — never
	 * interactive, so they must not arm the reveal fallback; the cover
	 * holds until the real window arrives. */
	if (client_is_unmanaged(c))
		return;
	if (!fx.window_ms)
		fx.window_ms = monotonic_msec();
}

/* Belt-and-braces from rendermon: a fullscreen game is classified and
 * producing frames on some monitor.  The reveal additionally waits for
 * content_seen — the game must have committed an actual buffer under the
 * cover, so the screen stays black until something real is there.
 * rendermon retries every vblank, so the reveal lands on the first frame
 * after the commit arrives. */
void
launchfx_game_ready(void)
{
	if (!fx.active)
		return;
	if (!fx.content_seen) {
		fx.reveal_pending = 1;
		return;
	}
	if (fx.grown)
		fx_finish();
	else
		fx.reveal_pending = 1;
}

/* Every surface commit passes through here (animcommitnotify).  While a
 * cover is up, the first buffer commit from a fullscreen game marks the
 * content the reveal is waiting for. */
void
launchfx_note_commit(Client *c)
{
	struct wlr_surface *surf;

	if (!fx.active || fx.content_seen || !c || !c->isfullscreen)
		return;
	if (!looks_like_game(c))
		return;
	surf = client_surface(c);
	if (!surf || !(surf->current.committed & WLR_SURFACE_STATE_BUFFER))
		return;
	fx.content_seen = 1;
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

	if (fx.active) {
		uint64_t now = monotonic_msec();

		/* Track the launch chain: the reaper wrapper lives exactly as
		 * long as its stage.  Setup stages die and are replaced (the
		 * scan below adopts the successor); only a chain that ends
		 * with no successor and no window means abort/crash. */
		if (fx.reaper > 0 && kill(fx.reaper, 0) != 0) {
			fx.reaper = 0;
			fx.orphan_ms = now;
		}
		if (!fx.reaper && fx.orphan_ms) {
			if (fx.window_ms) {
				/* Game exited/crashed during load */
				fx_finish();
			} else if (now - fx.orphan_ms > FX_CHAIN_GRACE_MS) {
				/* Chain ended without any window: aborted */
				fx_finish();
			}
		}

		/* Interactive launcher fallback: a game window has been up
		 * this long without going fullscreen — the user needs to see
		 * and click it. */
		if (fx.active && fx.window_ms &&
				now - fx.window_ms > FX_LAUNCHER_MS)
			fx_finish();
	}

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
				if (fx.active) {
					/* Next stage of the same Play press —
					 * adopt it, keep the one cover up. */
					fx.reaper = pid;
					fx.orphan_ms = 0;
				} else {
					launchfx_start(pid);
				}
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
