/*
 * launchfx.c — instant Steam-launch reaction.
 *
 *  1. Detect the Play press by polling for Steam's `reaper` launch
 *     wrapper (spawned the moment Play is clicked, seconds before any
 *     window exists).
 *  2. Pre-boost: CPU governor → performance immediately, so Proton
 *     setup / shader compilation / asset loading run at full clock
 *     from t=0.  Full ultra game mode takes over when the window maps.
 *  3. Launch animation: a tiny black dot at the center of the monitor
 *     the game takes grows smoothly until it covers the screen, holds
 *     black through the last of the load, and is dropped the moment the
 *     game presents its first fullscreen frame.  It starts when the game
 *     enters fullscreen (setfullscreen), NOT at the Play press: Steam's
 *     splash/launcher stages run for seconds before that, and covering
 *     them meant the black came up, timed out and replayed.
 */
#include "nixlytile.h"
#include "client.h"

#include <cairo/cairo.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <unistd.h>

#define FX_POLL_MS      200
#define FX_TICK_MS      16
#define FX_GROW_MS      500.0
#define FX_HEADSTART_MS 1000  /* cover plays this long before ultra mode */
#define FX_DOT_TEX      256   /* circle texture size; scaled up when drawn */
#define FX_WATCHDOG_MS  45000
/* Hard cap on how long the black may hold once the cover is up.  The
 * reveal normally comes from the game's first fullscreen commit; this is
 * only the "it never presented" escape. */
#define FX_COVER_MAX_MS 20000
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
	struct wlr_scene_rect *black;
	struct wlr_buffer *dot_buf;
	uint64_t start_ms;
	struct wl_event_source *tick;
	struct wl_event_source *watchdog;
} fx;

static struct wl_event_source *fx_poll_timer;
static pid_t seen_reapers[32];
static int seen_reaper_count;

/* Klient som ba om fullskjerm mens coveret fortsatt vokste: selve flippen
 * er utsatt til sirkelen dekker skjermen, så overgangen (og spillets første
 * blanke/hvite frame) alltid skjer under svart — ikke samtidig med
 * animasjonen. */
static Client *fx_pending_fs;

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
	fx.black = NULL;
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

	/* Aldri la en utsatt fullskjerm-flipp dø med coveret (watchdog/feil)
	 * — da ble spillet stående i vindu. Re-entry er trygt: fx.tree er
	 * borte og fx_covered satt, så deferren slipper flippen rett gjennom. */
	if (fx_pending_fs) {
		Client *p = fx_pending_fs;
		fx_pending_fs = NULL;
		setfullscreen(p, 1);
	}
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
	static const float black_col[4] = { 0.0f, 0.0f, 0.0f, 1.0f };

	if (!fx.mon)
		return;
	/* Crisp full-monitor rect replaces the scaled circle. */
	fx.black = wlr_scene_rect_create(fx.tree,
			fx.mon->m.width, fx.mon->m.height, black_col);
	if (fx.black)
		wlr_scene_node_set_position(&fx.black->node, 0, 0);
	if (fx.dot) {
		wlr_scene_node_destroy(&fx.dot->node);
		fx.dot = NULL;
	}
	if (fx.tick) {
		wl_event_source_remove(fx.tick);
		fx.tick = NULL;
	}
	fx.grown = 1;
	/* Svart dekker nå hele skjermen — utfør fullskjerm-flippen som
	 * ventet på det. Re-entry gjennom deferren slipper gjennom fordi
	 * fx.grown er satt. */
	if (fx_pending_fs) {
		Client *p = fx_pending_fs;
		fx_pending_fs = NULL;
		setfullscreen(p, 1);
	}
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

	/* Sirkelen ender på skjermens DIAGONAL og stikker da utenfor alle
	 * kanter — uklippet tegnes overhenget på naboskjermene. Kutt
	 * senterutsnittet mot skjermstørrelsen med en source box. */
	{
		int vw = MIN(di, fx.mon->m.width);
		int vh = MIN(di, fx.mon->m.height);

		if (vw < di || vh < di) {
			double s = (double)fx.dot_buf->width / (double)di;
			struct wlr_fbox src = {
				.x = (di - vw) / 2.0 * s,
				.y = (di - vh) / 2.0 * s,
				.width = vw * s,
				.height = vh * s,
			};
			wlr_scene_buffer_set_source_box(fx.dot, &src);
		} else {
			wlr_scene_buffer_set_source_box(fx.dot, NULL);
		}
		wlr_scene_buffer_set_dest_size(fx.dot, vw, vh);
		wlr_scene_node_set_position(&fx.dot->node,
				(fx.mon->m.width - vw) / 2,
				(fx.mon->m.height - vh) / 2);
	}
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

	{
		const char *argv[] = { "nixly-prewarm", "readahead", appid, NULL };
		spawn_cmd_async(argv);
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

	/* The game is fullscreen now, so the "window mapped but never
	 * fullscreened" reveal no longer applies — drop it and let the
	 * watchdog cap the black instead. */
	fx.window_ms = 0;
	if (fx.watchdog)
		wl_event_source_timer_update(fx.watchdog, FX_COVER_MAX_MS);

	/* The one game-mode message, timed with the intro animation.  Forced
	 * past the osd_show() gate, which drops toasts during a launch. */
	osd_show_force(m, "Game Mode On");

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

	/* No visual here.  The Play press is only the pre-boost trigger —
	 * Steam's splash, install and Proton stages run for seconds before
	 * the game exists, and a cover started here timed out and replayed
	 * on top of them.  The cover starts at the fullscreen transition. */
	wlr_log(WLR_INFO, "launchfx: Steam launch detected (reaper pid %d) — "
			"pre-boost; cover plays at the fullscreen transition",
			(int)reaper);
}

/* Follow the game if it fullscreens on a monitor other than the one the
 * cover was started on. */
static void
fx_move_cover(Monitor *m)
{
	if (!fx.tree || !m || m == fx.mon)
		return;
	fx.mon = m;
	wlr_scene_node_set_position(&fx.tree->node, m->m.x, m->m.y);
	if (fx.black)
		wlr_scene_rect_set_size(fx.black, m->m.width, m->m.height);
	if (m->wlr_output)
		wlr_output_schedule_frame(m->wlr_output);
}

/* The cover trigger.  Called from setfullscreen() the moment a game takes
 * the whole output — before the compositor has configured it, so well
 * before the game presents its first fullscreen frame — and again from
 * update_game_mode() as a backstop for games that reach ultra mode
 * without a fullscreen transition of their own (returning to the tag,
 * X11 override-redirect fullscreen).  Once per client: c->fx_covered.
 * rendermon's launchfx_game_ready() drops the black again. */
void
launchfx_fullscreen_starting(Client *c)
{
	if (!c || !c->mon || !c->mon->wlr_output)
		return;
	if (!looks_like_game(c))
		return;
	/* Cover already up (the game moved output, or the backstop fired
	 * after the setfullscreen trigger) — just keep it on the right one. */
	if (fx.tree) {
		fx_move_cover(c->mon);
		c->fx_covered = 1;
		return;
	}
	if (c->fx_covered)
		return;
	c->fx_covered = 1;
	/* No Play press seen (non-Steam launcher): start tracking now so the
	 * pre-boost and the abort watchdog exist for this cover too. */
	if (!fx.active)
		fx_track_start();
	if (!fx_start_cover(c->mon))
		return;

	wlr_log(WLR_INFO, "launchfx: game going fullscreen — cover on %s",
			c->mon->wlr_output->name);
}

/* Kalles fra toppen av setfullscreen(c, 1) FØR noe state endres.
 * Returnerer 1 hvis flippen skal utsettes: coveret startes/vokser, og
 * setfullscreen kalles igjen herfra når svart dekker skjermen. Uten
 * dette skjer flippen samtidig med animasjonsstarten — spillets første
 * (ofte hvite) frame legger seg bak sirkelen i stedet for desktopen. */
int
launchfx_defer_fullscreen(Client *c)
{
	if (!c || !c->mon || !c->mon->wlr_output)
		return 0;
	if (!looks_like_game(c))
		return 0;

	if (fx.tree) {
		if (fx.grown)
			return 0; /* svart står allerede — flipp med en gang */
		fx_pending_fs = c;
		return 1;
	}

	if (c->fx_covered)
		return 0; /* dekket én gang — ingen replay, ingen utsettelse */

	launchfx_fullscreen_starting(c);
	if (!fx.tree || fx.grown)
		return 0; /* cover kunne ikke bygges — ikke blokker flippen */
	fx_pending_fs = c;
	return 1;
}

/* Klienten forsvant (unmap/destroy) eller forlot fullskjerm-ønsket —
 * ikke flipp en død/angrende klient når coveret er ferdig vokst. */
void
launchfx_forget_client(Client *c)
{
	if (fx_pending_fs == c)
		fx_pending_fs = NULL;
}

/* A launch is being tracked (pre-boost and/or cover up). */
int
launchfx_active(void)
{
	return fx.active;
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

/* The /proc reaper sweep runs on its own thread: ~600 pids × open/read/
 * close of /proc/PID/comm every 200 ms is milliseconds of main-thread
 * time for the whole life of the Steam process.  The worker publishes
 * candidate pids through a pipe; all launch state (seen_reapers, fx)
 * stays on the compositor thread. */
static int fxw_pipe[2] = { -1, -1 };
static pthread_mutex_t fxw_lock = PTHREAD_MUTEX_INITIALIZER;
static pid_t fxw_found[32];
static int fxw_found_count;
static volatile int fxw_scan_enabled;

static void *
fxw_worker(void *arg)
{
	DIR *dir;
	struct dirent *ent;

	(void)arg;
	pthread_setname_np(pthread_self(), "nixly-launchfx");
	for (;;) {
		if (fxw_scan_enabled && (dir = opendir("/proc"))) {
			pid_t found[32];
			int n = 0;
			while ((ent = readdir(dir)) && n < (int)LENGTH(found)) {
				pid_t pid;
				if (ent->d_name[0] < '0' || ent->d_name[0] > '9')
					continue;
				pid = (pid_t)atoi(ent->d_name);
				if (!proc_comm_is(pid, "reaper"))
					continue;
				found[n++] = pid;
			}
			closedir(dir);
			if (n > 0) {
				pthread_mutex_lock(&fxw_lock);
				memcpy(fxw_found, found,
					(size_t)n * sizeof(found[0]));
				fxw_found_count = n;
				pthread_mutex_unlock(&fxw_lock);
				(void)!write(fxw_pipe[1], "r", 1);
			}
		}
		usleep(FX_POLL_MS * 1000);
	}
	return NULL;
}

static int
fxw_event(int fd, uint32_t mask, void *data)
{
	char drain[16];
	pid_t found[32];
	int n, i;

	(void)mask; (void)data;
	while (read(fd, drain, sizeof(drain)) > 0)
		;
	pthread_mutex_lock(&fxw_lock);
	n = fxw_found_count;
	memcpy(found, fxw_found, (size_t)n * sizeof(found[0]));
	pthread_mutex_unlock(&fxw_lock);

	for (i = 0; i < n; i++) {
		pid_t pid = found[i];
		if (reaper_seen(pid))
			continue;
		if (seen_reaper_count < (int)LENGTH(seen_reapers))
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
	return 0;
}

static int
fx_poll_cb(void *data)
{
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

	/* While a game is in game mode and no launch is being tracked there
	 * is no Play press to detect — stop the worker's /proc sweep so
	 * nothing runs mid-game.  Resumes when game mode exits. */
	if (game_mode_active && !fx.active)
		steam_up = 0;

	/* The sweep itself runs on the fxw_worker thread; results arrive
	 * via fxw_event.  Here we only gate it. */
	fxw_scan_enabled = steam_up;

	wl_event_source_timer_update(fx_poll_timer, FX_POLL_MS);
	return 0;
}

void
launchfx_init(void)
{
	fx_poll_timer = wl_event_loop_add_timer(event_loop, fx_poll_cb, NULL);
	if (fx_poll_timer)
		wl_event_source_timer_update(fx_poll_timer, FX_POLL_MS);

	if (pipe2(fxw_pipe, O_CLOEXEC | O_NONBLOCK) == 0) {
		pthread_t tid;
		pthread_attr_t attr;
		wl_event_loop_add_fd(event_loop, fxw_pipe[0],
				WL_EVENT_READABLE, fxw_event, NULL);
		pthread_attr_init(&attr);
		pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
		if (pthread_create(&tid, &attr, fxw_worker, NULL) != 0)
			wlr_log(WLR_ERROR, "launchfx: sweep thread start failed — "
				"Play-press detection disabled");
		pthread_attr_destroy(&attr);
	}
}
